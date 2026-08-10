/* SIGSTOP / SIGCONT to a process the policy gate already cleared.
 *
 * The gate decided WHETHER. This decides whether the process is still the one
 * that was decided about — and that second question cannot be answered
 * earlier, because a pid can be recycled between the decision and the syscall.
 * Everything here exists for that gap. */

#define _POSIX_C_SOURCE 200809L

#include "geistshell/machine_executor.h"

#include "geistshell/machine_fixture.h"
#include "geistshell/sexpr.h"

#include <string.h>

/* Linux only, and not for portability reasons: the identity re-check reads
 * /proc/<pid>/stat, and without it a signal goes out on a pid that may have
 * been recycled since the snapshot. An executor that cannot verify what it is
 * about to stop must not stop anything, so every other platform reports
 * UNSUPPORTED rather than acting blind. */
#if defined(__linux__)
#    include <errno.h>
#    include <signal.h>
#    include <stdio.h>
#    include <unistd.h>
#    define SPG_MACHINE_SIGNALS 1
#endif

const char *spg_machine_exec_outcome_to_string(
    const enum spg_machine_exec_outcome outcome) {
    switch (outcome) {
    case SPG_MACHINE_EXEC_OK:
        return "ok";
    case SPG_MACHINE_EXEC_IDENTITY_CHANGED:
        return "identity_changed";
    case SPG_MACHINE_EXEC_GONE:
        return "gone";
    case SPG_MACHINE_EXEC_FORBIDDEN:
        return "forbidden";
    case SPG_MACHINE_EXEC_NOT_FOUND:
        return "not_found";
    case SPG_MACHINE_EXEC_REFUSED:
        return "refused";
    case SPG_MACHINE_EXEC_UNSUPPORTED:
        return "unsupported";
    }
    return "unsupported";
}

static const struct spg_process_sample *
find_target(const struct spg_machine_state *machine, const char *target) {
    if (machine == nullptr || target == nullptr || target[0] == '\0') {
        return nullptr;
    }
    for (size_t i = 0u; i < machine->n_processes; i += 1u) {
        if (strcmp(machine->processes[i].profile_id, target) == 0) {
            return &machine->processes[i];
        }
    }
    return nullptr;
}

#if defined(SPG_MACHINE_SIGNALS)
/* Re-read the identity from /proc immediately before signalling. Returns false
 * when the pid is gone or now belongs to somebody else.
 *
 * This is deliberately a fresh read rather than a cached value: the snapshot
 * could be a tick old, and a tick is long enough for a pid to be reused. */
static bool identity_still_matches(const uint64_t pid,
                                   const uint64_t start_identity) {
    char path[64];
    if (snprintf(path, sizeof path, "/proc/%llu/stat",
                 (unsigned long long)pid) < 0) {
        return false;
    }
    FILE *f = fopen(path, "rbe");
    if (f == nullptr) {
        return false;
    }
    char         buf[2048];
    const size_t n = fread(buf, 1u, sizeof buf, f);
    (void)fclose(f);
    if (n == 0u) {
        return false;
    }
    struct spg_process_sample now = {};
    if (spg_process_parse_stat(n, buf, 4096u, &now) != SPG_OK) {
        return false;
    }
    return now.pid == pid && now.start_identity == start_identity;
}
#endif

#if defined(SPG_MACHINE_SIGNALS)
/* Fork a guardian that outlives us if we are killed.
 *
 * It holds the read end of a pipe and blocks on it. Two things can happen:
 * a byte arrives (we released the pause ourselves) and it exits without acting,
 * or the pipe closes because we died and it resumes the process. No timer, no
 * heartbeat, no state — the kernel closing our fds IS the signal.
 *
 * Returns the write end, or -1 when no guardian could be created. A pause with
 * no guardian is refused by the caller: an unguarded stop is exactly the
 * damage this is here to prevent. */
static int spawn_guardian(const uint64_t pid, const uint64_t start_identity) {
    int fds[2];
    if (pipe(fds) != 0) {
        return -1;
    }
    const pid_t child = fork();
    if (child < 0) {
        (void)close(fds[0]);
        (void)close(fds[1]);
        return -1;
    }
    if (child == 0) {
        (void)close(fds[1]);
        char    byte = 0;
        ssize_t n    = 0;
        do {
            n = read(fds[0], &byte, 1u);
        } while (n < 0 && errno == EINTR);
        if (n == 0) {
            /* The parent died without releasing. Re-check identity first: a
             * guardian that wakes a recycled pid is worse than one that does
             * nothing. */
            if (identity_still_matches(pid, start_identity)) {
                (void)kill((pid_t)pid, SIGCONT);
            }
        }
        _exit(0);
    }
    (void)close(fds[0]);
    return fds[1];
}

/* Tell a guardian to stand down. Writing the byte is what distinguishes "we
 * handled it" from "we died"; closing alone would look like death. */
static void dismiss_guardian(int *fd) {
    if (fd == nullptr || *fd < 0) {
        return;
    }
    const char byte = 1;
    ssize_t    n    = 0;
    do {
        n = write(*fd, &byte, 1u);
    } while (n < 0 && errno == EINTR);
    (void)close(*fd);
    *fd = -1;
}
#endif

/* Ledger bookkeeping. A pause that cannot be recorded is not performed: the
 * alternative is a stopped process nobody owes a resume for. */
static bool ledger_remember(struct spg_machine_pause_ledger *ledger,
                            const struct spg_process_sample *p) {
    if (ledger == nullptr) {
        return false;
    }
    for (size_t i = 0u; i < ledger->count; i += 1u) {
        if (ledger->entries[i].pid == p->pid &&
            ledger->entries[i].start_identity == p->start_identity) {
            return true; /* pausing twice still owes exactly one resume */
        }
    }
    if (ledger->count >= SPG_MACHINE_MAX_PAUSED) {
        return false;
    }
    struct spg_machine_paused_entry *e = &ledger->entries[ledger->count];
    e->pid                             = p->pid;
    e->start_identity                  = p->start_identity;
    memcpy(e->profile_id, p->profile_id, SPG_PROCESS_ID_CAP);
    e->profile_id[SPG_PROCESS_ID_CAP - 1u] = '\0';
    e->guard_fd                            = -1;
    ledger->count += 1u;
    return true;
}

#if defined(SPG_MACHINE_SIGNALS)
static void ledger_set_guard(struct spg_machine_pause_ledger *ledger,
                             const uint64_t pid, const uint64_t start_identity,
                             const int fd) {
    if (ledger == nullptr) {
        return;
    }
    for (size_t i = 0u; i < ledger->count; i += 1u) {
        if (ledger->entries[i].pid == pid &&
            ledger->entries[i].start_identity == start_identity) {
            ledger->entries[i].guard_fd = fd;
            return;
        }
    }
}
#endif

static void ledger_forget(struct spg_machine_pause_ledger *ledger,
                          const uint64_t pid, const uint64_t start_identity) {
    if (ledger == nullptr) {
        return;
    }
    for (size_t i = 0u; i < ledger->count; i += 1u) {
        if (ledger->entries[i].pid == pid &&
            ledger->entries[i].start_identity == start_identity) {
#if defined(SPG_MACHINE_SIGNALS)
            /* Dismissed, not just dropped: a guardian left waiting would
             * resume this process again when we exit normally. */
            dismiss_guardian(&ledger->entries[i].guard_fd);
#endif
            ledger->entries[i] = ledger->entries[ledger->count - 1u];
            ledger->count -= 1u;
            return;
        }
    }
}

static enum spg_machine_exec_outcome
send_signal(const uint64_t pid, const uint64_t start_identity,
            const enum spg_action_kind kind) {
#if defined(SPG_MACHINE_SIGNALS)
    /* Never the agent itself, never init, never a wildcard: kill(0, ...) hits
     * the whole process group and kill(-1, ...) hits everything the user owns.
     * Neither is expressible through the profile, but the executor is the last
     * line and does not assume the layers above it are perfect. */
    if (pid <= 1u || pid == (uint64_t)getpid() || pid == (uint64_t)getppid() ||
        pid > (uint64_t)INT32_MAX) {
        return SPG_MACHINE_EXEC_REFUSED;
    }
    if (!identity_still_matches(pid, start_identity)) {
        /* Distinguish "it left" from "somebody else has the pid now": the
         * first is normal, the second is the one worth alarming about. */
        return kill((pid_t)pid, 0) == 0 ? SPG_MACHINE_EXEC_IDENTITY_CHANGED
                                        : SPG_MACHINE_EXEC_GONE;
    }
    const int sig = kind == SPG_ACTION_MACHINE_PAUSE ? SIGSTOP : SIGCONT;
    if (kill((pid_t)pid, sig) == 0) {
        return SPG_MACHINE_EXEC_OK;
    }
    switch (errno) {
    case ESRCH:
        return SPG_MACHINE_EXEC_GONE;
    case EPERM:
        return SPG_MACHINE_EXEC_FORBIDDEN;
    default:
        return SPG_MACHINE_EXEC_REFUSED;
    }
#else
    (void)pid;
    (void)start_identity;
    (void)kind;
    return SPG_MACHINE_EXEC_UNSUPPORTED;
#endif
}

static void
journal_action(const struct spg_machine_executor_state  *state,
               const struct spg_machine_executor_config *config,
               const enum spg_action_kind kind, const char *target,
               const struct spg_machine_executor_workspace *workspace,
               struct spg_machine_executor_result          *result) {
    struct spg_sexpr_writer w;
    spg_sexpr_writer_init(&w, workspace->payload_capacity, workspace->payload);
    (void)spg_sexpr_writer_append_text(&w, "(machine_action (kind ");
    (void)spg_sexpr_writer_append_text(&w, spg_action_kind_to_string(kind));
    (void)spg_sexpr_writer_append_text(&w, ") (target \"");
    (void)spg_sexpr_writer_append_text(&w, target);
    (void)spg_sexpr_writer_append_text(&w, "\") (pid ");
    (void)spg_sexpr_writer_append_u64(&w, result->pid);
    /* Recorded so a later run can re-validate before resuming: recovery would
     * otherwise have only a pid, and a pid alone is not an identity. */
    (void)spg_sexpr_writer_append_text(&w, ") (identity ");
    (void)spg_sexpr_writer_append_u64(&w, result->identity);
    (void)spg_sexpr_writer_append_text(&w, ") (outcome ");
    (void)spg_sexpr_writer_append_text(
        &w, spg_machine_exec_outcome_to_string(result->outcome));
    (void)spg_sexpr_writer_append_text(&w, "))");
    result->payload_used      = w.used;
    result->payload_truncated = w.truncated;

    if (!config->write_journal || state->journal == nullptr) {
        return;
    }
    /* Journalled whatever the outcome. A refusal that leaves no record is a
     * refusal nobody can audit. */
    uint64_t sequence = 0u;
    (void)spg_journal_writer_append(
        state->journal, config->timestamp_ns, config->parent_sequence,
        SPG_JOURNAL_EVENT_ACTION,
        result->outcome == SPG_MACHINE_EXEC_OK ? SPG_OK : SPG_E_INVALID_STATE,
        w.used, (const uint8_t *)workspace->payload, &sequence);
    result->sequence = sequence;
}

enum spg_status spg_machine_executor_step(
    const struct spg_machine_executor_state  *state,
    const struct spg_machine_executor_config *config,
    const enum spg_action_kind kind, const char *target,
    const struct spg_machine_executor_workspace *workspace,
    struct spg_machine_executor_result          *result) {
    if (state == nullptr || config == nullptr || workspace == nullptr ||
        result == nullptr || workspace->payload == nullptr ||
        workspace->payload_capacity == 0u || target == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    if (kind != SPG_ACTION_MACHINE_PAUSE && kind != SPG_ACTION_MACHINE_RESUME) {
        return SPG_E_INVALID_ARG;
    }
    *result = (struct spg_machine_executor_result){};

    const struct spg_process_sample *p = find_target(state->machine, target);
    if (p == nullptr) {
        result->outcome = SPG_MACHINE_EXEC_NOT_FOUND;
    } else if (!config->execution_enabled) {
        result->pid     = p->pid;
        result->outcome = SPG_MACHINE_EXEC_UNSUPPORTED;
    } else if (kind == SPG_ACTION_MACHINE_PAUSE &&
               !ledger_remember(state->ledger, p)) {
        /* No room to record it, or nowhere to record it at all. Stopping a
         * process we could not promise to restart is exactly the outcome this
         * phase exists to prevent, so the pause does not happen. */
        result->pid      = p->pid;
        result->identity = p->start_identity;
        result->outcome  = SPG_MACHINE_EXEC_REFUSED;
    } else {
        result->pid      = p->pid;
        result->identity = p->start_identity;
#if defined(SPG_MACHINE_SIGNALS)
        if (kind == SPG_ACTION_MACHINE_PAUSE) {
            /* Armed BEFORE the stop, not after: a crash in between would
             * otherwise leave exactly the stopped-and-forgotten process the
             * guardian exists to prevent. */
            const int guard = spawn_guardian(p->pid, p->start_identity);
            if (guard < 0) {
                ledger_forget(state->ledger, p->pid, p->start_identity);
                result->outcome = SPG_MACHINE_EXEC_REFUSED;
                journal_action(state, config, kind, target, workspace, result);
                return SPG_OK;
            }
            ledger_set_guard(state->ledger, p->pid, p->start_identity, guard);
        }
#endif
        result->outcome = send_signal(p->pid, p->start_identity, kind);
        if (kind == SPG_ACTION_MACHINE_PAUSE &&
            result->outcome != SPG_MACHINE_EXEC_OK) {
            ledger_forget(state->ledger, p->pid, p->start_identity);
        }
        if (kind == SPG_ACTION_MACHINE_RESUME &&
            result->outcome == SPG_MACHINE_EXEC_OK) {
            ledger_forget(state->ledger, p->pid, p->start_identity);
        }
    }
    journal_action(state, config, kind, target, workspace, result);
    return SPG_OK;
}

enum spg_status spg_machine_executor_run(
    const struct spg_machine_executor_state  *state,
    const struct spg_machine_executor_config *config, const size_t n,
    const struct spg_machine_action              actions[],
    const struct spg_machine_executor_workspace *workspace,
    struct spg_machine_executor_result results[], size_t *out_done) {
    if (out_done == nullptr ||
        (n > 0u && (actions == nullptr || results == nullptr))) {
        return SPG_E_INVALID_ARG;
    }
    *out_done = 0u;
    for (size_t i = 0u; i < n; i += 1u) {
        const enum spg_status status = spg_machine_executor_step(
            state, config, actions[i].kind, actions[i].target, workspace,
            &results[i]);
        if (status != SPG_OK) {
            return status;
        }
        *out_done += 1u;
        if (results[i].outcome != SPG_MACHINE_EXEC_OK) {
            break; /* a batch stops at the first refusal, so a later action
                    * cannot depend on an effect that never happened */
        }
    }
    return SPG_OK;
}

/* --- phase 6b: nothing stays paused ------------------------------------- */

/* Resume one entry directly, without a snapshot: at cleanup time the snapshot
 * is stale by definition, and the ledger is the record of what we owe. */
static enum spg_machine_exec_outcome
resume_entry(const struct spg_machine_paused_entry       *entry,
             const struct spg_machine_executor_state     *state,
             const struct spg_machine_executor_config    *config,
             const struct spg_machine_executor_workspace *workspace,
             struct spg_machine_executor_result          *result) {
    *result = (struct spg_machine_executor_result){
        .pid = entry->pid, .identity = entry->start_identity};
    result->outcome = config->execution_enabled
                          ? send_signal(entry->pid, entry->start_identity,
                                        SPG_ACTION_MACHINE_RESUME)
                          : SPG_MACHINE_EXEC_UNSUPPORTED;
    journal_action(state, config, SPG_ACTION_MACHINE_RESUME, entry->profile_id,
                   workspace, result);
    return result->outcome;
}

enum spg_status spg_machine_ledger_release(
    struct spg_machine_pause_ledger             *ledger,
    const struct spg_machine_executor_state     *state,
    const struct spg_machine_executor_config    *config,
    const struct spg_machine_executor_workspace *workspace,
    size_t                                      *out_resumed) {
    if (state == nullptr || config == nullptr || workspace == nullptr ||
        workspace->payload == nullptr || workspace->payload_capacity == 0u ||
        out_resumed == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out_resumed = 0u;
    if (ledger == nullptr) {
        return SPG_OK;
    }
    /* Every entry is attempted even if one fails: a process that exited on its
     * own must not stop us from restarting the next one. */
    for (size_t i = 0u; i < ledger->count; i += 1u) {
        struct spg_machine_executor_result r = {};
        if (resume_entry(&ledger->entries[i], state, config, workspace, &r) ==
            SPG_MACHINE_EXEC_OK) {
            *out_resumed += 1u;
        }
#if defined(SPG_MACHINE_SIGNALS)
        dismiss_guardian(&ledger->entries[i].guard_fd);
#endif
    }
    ledger->count = 0u;
    return SPG_OK;
}

/* --- recovery from a journal -------------------------------------------- */

/* One pid's outstanding debt while walking the journal. */
struct pending_pause {
    uint64_t pid;
    uint64_t identity;
    char     profile_id[SPG_PROCESS_ID_CAP];
};

static void pending_add(struct pending_pause pending[static 1], size_t *count,
                        const struct pending_pause *entry) {
    for (size_t i = 0u; i < *count; i += 1u) {
        if (pending[i].pid == entry->pid &&
            pending[i].identity == entry->identity) {
            return;
        }
    }
    if (*count < SPG_MACHINE_MAX_PAUSED) {
        pending[*count] = *entry;
        *count += 1u;
    }
}

static void pending_remove(struct pending_pause pending[static 1],
                           size_t *count, const uint64_t pid,
                           const uint64_t identity) {
    for (size_t i = 0u; i < *count; i += 1u) {
        if (pending[i].pid == pid && pending[i].identity == identity) {
            pending[i] = pending[*count - 1u];
            *count -= 1u;
            return;
        }
    }
}

/* Pull one unsigned field out of a (machine_action ...) payload. The payload is
 * written by journal_action above, so this is parsing our own format — a full
 * s-expression parse would need a node arena for no gain. */
static bool payload_u64(const size_t n, const char text[], const char *field,
                        uint64_t *out) {
    char needle[32];
    if (snprintf(needle, sizeof needle, "(%s ", field) < 0) {
        return false;
    }
    const size_t needle_n = strlen(needle);
    for (size_t i = 0u; i + needle_n < n; i += 1u) {
        if (memcmp(text + i, needle, needle_n) != 0) {
            continue;
        }
        size_t   j     = i + needle_n;
        uint64_t value = 0u;
        bool     any   = false;
        while (j < n && text[j] >= '0' && text[j] <= '9') {
            if (value > (UINT64_MAX - (uint64_t)(text[j] - '0')) / 10u) {
                return false;
            }
            value = value * 10u + (uint64_t)(text[j] - '0');
            any   = true;
            j += 1u;
        }
        if (any) {
            *out = value;
            return true;
        }
    }
    return false;
}

static bool payload_has(const size_t n, const char text[], const char *what) {
    const size_t w = strlen(what);
    for (size_t i = 0u; i + w <= n; i += 1u) {
        if (memcmp(text + i, what, w) == 0) {
            return true;
        }
    }
    return false;
}

enum spg_status spg_machine_recover_journal(
    const char *journal_path, const struct spg_machine_executor_config *config,
    const struct spg_machine_executor_workspace *workspace,
    struct spg_journal_writer *journal, size_t *out_resumed) {
    if (journal_path == nullptr || config == nullptr || workspace == nullptr ||
        workspace->payload == nullptr || out_resumed == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out_resumed = 0u;

    struct spg_journal_reader reader = {};
    if (spg_journal_reader_open(&reader, journal_path) != SPG_OK) {
        return SPG_OK; /* no journal, nothing owed */
    }
    struct pending_pause pending[SPG_MACHINE_MAX_PAUSED] = {};
    size_t               pending_count                   = 0u;

    struct spg_journal_record record = {};
    uint8_t                   raw[1024];
    while (spg_journal_reader_next(&reader, sizeof raw, raw, &record) ==
           SPG_OK) {
        const char  *payload   = (const char *)raw;
        const size_t payload_n = record.payload_used;
        if (record.header.event_kind != (uint32_t)SPG_JOURNAL_EVENT_ACTION ||
            !payload_has(payload_n, payload, "(machine_action ")) {
            continue;
        }
        if (!payload_has(payload_n, payload, "(outcome ok)")) {
            continue; /* it never landed, so nothing is owed */
        }
        struct pending_pause entry = {};
        if (!payload_u64(payload_n, payload, "pid", &entry.pid) ||
            !payload_u64(payload_n, payload, "identity", &entry.identity)) {
            continue;
        }
        if (payload_has(payload_n, payload, "machine_pause_process")) {
            pending_add(pending, &pending_count, &entry);
        } else if (payload_has(payload_n, payload, "machine_resume_process")) {
            pending_remove(pending, &pending_count, entry.pid, entry.identity);
        }
    }
    (void)spg_journal_reader_close(&reader);

    const struct spg_machine_executor_state state = {.journal = journal};
    for (size_t i = 0u; i < pending_count; i += 1u) {
        struct spg_machine_executor_result r = {};
        struct spg_machine_paused_entry    e = {
            .pid = pending[i].pid, .start_identity = pending[i].identity};
        /* The id is not needed to signal, only to journal what was restored. */
        memcpy(e.profile_id, "recovered", sizeof "recovered");
        if (resume_entry(&e, &state, config, workspace, &r) ==
            SPG_MACHINE_EXEC_OK) {
            *out_resumed += 1u;
        }
    }
    return SPG_OK;
}
