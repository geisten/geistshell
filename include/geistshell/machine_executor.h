#ifndef GEISTSHELL_MACHINE_EXECUTOR_H
#define GEISTSHELL_MACHINE_EXECUTOR_H

#include "geistshell/journal.h"
#include "geistshell/machine_state.h"
#include "geistshell/policy.h"
#include "geistshell/recommendation.h"
#include "geistshell/status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Executes an ALLOW'd machine action (roadmap phase 6, #66): SIGSTOP or
 * SIGCONT to a process the policy gate already cleared.
 *
 * No shell, no system(), no popen(). The model never produces a command for
 * this path — it names a profile id, and the id is resolved here against the
 * snapshot the gate saw. */

enum spg_machine_exec_outcome {
    SPG_MACHINE_EXEC_OK = 0,
    /* The pid is still there but is no longer the process we observed. This is
     * the case the whole identity apparatus exists for: pids get recycled, and
     * signalling the new owner would be the worst bug this module could have.
     */
    SPG_MACHINE_EXEC_IDENTITY_CHANGED,
    SPG_MACHINE_EXEC_GONE,        /* ESRCH: it exited on its own */
    SPG_MACHINE_EXEC_FORBIDDEN,   /* EPERM: not ours to signal */
    SPG_MACHINE_EXEC_NOT_FOUND,   /* the target id is not in the snapshot */
    SPG_MACHINE_EXEC_REFUSED,     /* self, pid 1, or a nonsensical pid */
    SPG_MACHINE_EXEC_UNSUPPORTED, /* no process signals on this platform */
};

/* Every pause this run performed and has not undone (roadmap phase 6b, #80).
 *
 * A pause is only reversible while somebody remembers it happened. Without
 * this, a run that dies after SIGSTOP — crash, SIGKILL, budget end, an
 * experiment runner cut short — leaves the process stopped forever, and that
 * is the one way this runtime can do lasting damage without any policy being
 * violated. */
#define SPG_MACHINE_MAX_PAUSED 16u

struct spg_machine_paused_entry {
    uint64_t pid;
    /* Recorded so the release can refuse to signal a recycled pid. A cleanup
     * path that resumes strangers is worse than one that resumes nothing. */
    uint64_t start_identity;
    char     profile_id[SPG_PROCESS_ID_CAP];
};

struct spg_machine_pause_ledger {
    size_t                          count;
    struct spg_machine_paused_entry entries[SPG_MACHINE_MAX_PAUSED];
};

struct spg_machine_executor_state {
    /* The snapshot the decision was made on. The executor resolves the target
     * id here rather than trusting anything in the recommendation. */
    const struct spg_machine_state *machine;
    struct spg_journal_writer      *journal;
    /* Caller-owned, so the run that performs a pause is the run that owes the
     * resume. Null means pauses are not tracked — and an untracked pause is
     * refused rather than performed. */
    struct spg_machine_pause_ledger *ledger;
};

struct spg_machine_executor_config {
    uint32_t actor_id;
    uint64_t timestamp_ns;
    uint64_t parent_sequence;
    bool     write_journal;
    /* Off by default: a run that has not asked to touch the machine gets
     * SPG_MACHINE_EXEC_UNSUPPORTED rather than a signal. */
    bool execution_enabled;
};

struct spg_machine_executor_workspace {
    size_t payload_capacity; /* journal-event s-expression buffer */
    char  *payload;
};

struct spg_machine_executor_result {
    enum spg_machine_exec_outcome outcome;
    uint64_t                      pid;      /* what was signalled, or 0 */
    uint64_t                      identity; /* its start time, for recovery */
    uint64_t                      sequence; /* journal sequence, or 0 */
    size_t                        payload_used;
    bool                          payload_truncated;
};

/* Run one action. `target` is the profile id, NUL-terminated. */
[[nodiscard]] enum spg_status spg_machine_executor_step(
    const struct spg_machine_executor_state  *state,
    const struct spg_machine_executor_config *config, enum spg_action_kind kind,
    const char *target, const struct spg_machine_executor_workspace *workspace,
    struct spg_machine_executor_result *result);

/* Batch form, count before the array as everywhere else in the codebase.
 * `actions[]` rather than `actions[static n]`: an empty batch is legitimate
 * and [static 0] is undefined behaviour. Stops at the first action that fails
 * to execute; *out_done receives how many ran. */
struct spg_machine_action {
    enum spg_action_kind kind;
    const char          *target;
};

[[nodiscard]] enum spg_status
spg_machine_executor_run(const struct spg_machine_executor_state  *state,
                         const struct spg_machine_executor_config *config,
                         size_t n, const struct spg_machine_action actions[],
                         const struct spg_machine_executor_workspace *workspace,
                         struct spg_machine_executor_result           results[],
                         size_t                                      *out_done);

/* Resume everything still in the ledger, identity-checked, and empty it.
 * Called on every path out of a run — success, failure, max steps, budget,
 * policy denial, interrupt. Returns SPG_OK even when individual processes are
 * gone; *out_resumed counts the ones that were actually signalled. */
[[nodiscard]] enum spg_status spg_machine_ledger_release(
    struct spg_machine_pause_ledger             *ledger,
    const struct spg_machine_executor_state     *state,
    const struct spg_machine_executor_config    *config,
    const struct spg_machine_executor_workspace *workspace,
    size_t                                      *out_resumed);

/* Second line of defence, for the case the first cannot cover: SIGKILL and
 * power loss are not catchable, so a pause can outlive the process that owed
 * the resume. Reads a journal, pairs pauses with resumes, and resumes what was
 * left hanging — identity-checked, so a recycled pid is skipped rather than
 * woken.
 *
 * Returns SPG_OK on a missing or short journal (nothing to recover is not an
 * error); *out_resumed counts what was signalled. */
[[nodiscard]] enum spg_status spg_machine_recover_journal(
    const char *journal_path, const struct spg_machine_executor_config *config,
    const struct spg_machine_executor_workspace *workspace,
    struct spg_journal_writer *journal, size_t *out_resumed);

[[nodiscard]] const char *
spg_machine_exec_outcome_to_string(enum spg_machine_exec_outcome outcome);

#ifdef __cplusplus
}
#endif

#endif
