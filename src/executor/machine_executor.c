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
    } else {
        result->pid     = p->pid;
        result->outcome = send_signal(p->pid, p->start_identity, kind);
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
