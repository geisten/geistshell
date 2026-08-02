#ifndef GEISTSHELL_SHELL_EXECUTOR_H
#define GEISTSHELL_SHELL_EXECUTOR_H

#include "geistshell/executor_boundary.h"
#include "geistshell/journal.h"
#include "geistshell/policy.h"
#include "geistshell/recommendation.h"
#include "geistshell/status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Executes an ALLOW'd local_shell recommendation inside the governed pipeline:
 * gates the command through the shared executor boundary, runs it via the
 * bounded command executor, captures stdout/stderr, writes a human/agent
 * readable observation for the next step, and journals one
 * SPG_JOURNAL_EVENT_ACTION event so replay can reconstruct the run. Mirrors
 * spg_sim_executor_step / spg_mem_executor_step.
 *
 * A boundary denial or a command that fails to start is recorded in *result
 * (and the observation), not propagated as a step error. */

struct spg_shell_executor_state {
    struct spg_journal_writer *journal;
};

struct spg_shell_executor_config {
    uint32_t actor_id;
    uint64_t timestamp_ns;
    uint64_t parent_sequence;
    bool     write_journal;

    /* Boundary gating: command execution is only attempted when enabled, and
     * only inside working_dir (which must carry allowed_workdir_prefix). */
    bool        execution_enabled;
    const char *working_dir;
    const char *allowed_workdir_prefix;
    uint64_t    timeout_ms;
    size_t      max_stdout_bytes;
    size_t      max_stderr_bytes;
};

struct spg_shell_executor_workspace {
    size_t payload_capacity; /* journal-event s-expression buffer */
    char  *payload;

    /* The next-step observation (NUL-terminated): exit code + captured output,
     * or the denial/error reason. */
    size_t observation_capacity;
    char  *observation;

    /* Scratch for captured stdout/stderr. */
    size_t stdout_capacity;
    char  *stdout_buf;
    size_t stderr_capacity;
    char  *stderr_buf;
};

struct spg_shell_executor_result {
    bool                              approved;
    enum spg_executor_boundary_reason boundary_reason;

    enum spg_status run_status; /* outcome of the command executor */
    bool            started;
    bool            exited;
    int             exit_code;
    bool            timed_out;
    size_t          stdout_len;
    bool            stdout_truncated;

    size_t   observation_len;
    uint64_t action_sequence;
    size_t   payload_used;
    bool     payload_truncated;
};

/* text/text_n is the recommendation source (model output) that the
 * recommendation's command span indexes into. Returns SPG_E_INVALID_ARG on bad
 * arguments; otherwise SPG_OK with the outcome in *result. */
[[nodiscard]] enum spg_status spg_shell_executor_step(
    struct spg_shell_executor_state *state,
    const struct spg_shell_executor_config *config, size_t text_n,
    const char text[], const struct spg_recommendation *recommendation,
    const struct spg_policy_decision *policy_decision,
    const struct spg_shell_executor_workspace *workspace,
    struct spg_shell_executor_result *result);

#ifdef __cplusplus
}
#endif

#endif
