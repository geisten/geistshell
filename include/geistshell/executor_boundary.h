#ifndef GEISTSHELL_EXECUTOR_BOUNDARY_H
#define GEISTSHELL_EXECUTOR_BOUNDARY_H

#include "geistshell/policy.h"
#include "geistshell/recommendation.h"
#include "geistshell/status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum spg_executor_boundary_reason {
    SPG_EXECUTOR_BOUNDARY_OK = 0,
    SPG_EXECUTOR_BOUNDARY_EXECUTION_DISABLED,
    SPG_EXECUTOR_BOUNDARY_POLICY_DENIED,
    SPG_EXECUTOR_BOUNDARY_UNSUPPORTED_ACTION,
    SPG_EXECUTOR_BOUNDARY_NETWORK_FORBIDDEN,
    SPG_EXECUTOR_BOUNDARY_MISSING_COMMAND,
    SPG_EXECUTOR_BOUNDARY_BAD_WORKDIR,
    SPG_EXECUTOR_BOUNDARY_BAD_TIMEOUT,
    SPG_EXECUTOR_BOUNDARY_BAD_OUTPUT_LIMIT,
    SPG_EXECUTOR_BOUNDARY_ENV_NOT_CLEARED,
};

struct spg_executor_boundary_config {
    bool execution_enabled;

    const char *allowed_workdir_prefix;
    uint64_t    max_timeout_ms;
    size_t      max_stdout_bytes;
    size_t      max_stderr_bytes;

    bool require_clean_env;
};

struct spg_executor_boundary_request {
    const char *working_dir;
    uint64_t    timeout_ms;
    size_t      stdout_limit_bytes;
    size_t      stderr_limit_bytes;
    bool        env_cleared;
};

struct spg_executor_boundary_plan {
    bool approved;
    enum spg_executor_boundary_reason reason;
};

[[nodiscard]] enum spg_status spg_executor_boundary_check(
    const struct spg_executor_boundary_config *config,
    const struct spg_recommendation *recommendation,
    const struct spg_policy_decision *policy_decision,
    const struct spg_executor_boundary_request *request,
    struct spg_executor_boundary_plan *plan);

/* Gate a free-form local-shell command that has no backing model recommendation
 * or policy evaluation (the CLI `exec` command and the chat exec tool).
 * Synthesizes the VALID local-shell recommendation and ALLOW decision the
 * boundary expects -- so callers never forge policy-layer structs -- then runs
 * the same spg_executor_boundary_check. `command` is the argv[0] being gated (a
 * null or empty command yields a MISSING_COMMAND denial); uses_network marks
 * whether it reaches the network. config and request carry the sandbox limits
 * the caller enforces. */
[[nodiscard]] enum spg_status spg_executor_boundary_check_shell(
    const struct spg_executor_boundary_config *config, const char *command,
    bool uses_network, const struct spg_executor_boundary_request *request,
    struct spg_executor_boundary_plan *plan);

[[nodiscard]] const char *spg_executor_boundary_reason_to_string(
    enum spg_executor_boundary_reason reason);

#ifdef __cplusplus
}
#endif

#endif
