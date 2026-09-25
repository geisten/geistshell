/* realpath(3) is POSIX, but glibc gates it on __USE_MISC || __USE_XOPEN_EXTENDED,
 * which _POSIX_C_SOURCE does not set — same reason host_probe.c asks for
 * _DEFAULT_SOURCE. Without it -std=c23 leaves the function undeclared. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "geistshell/executor_boundary.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static void deny(struct spg_executor_boundary_plan *plan,
                 const enum spg_executor_boundary_reason reason) {
    plan->approved = false;
    plan->reason   = reason;
    plan->sandbox  = (struct spg_sandbox_spec){};
}

static void allow(struct spg_executor_boundary_plan         *plan,
                  const struct spg_executor_boundary_request *request) {
    plan->approved = true;
    plan->reason   = SPG_EXECUTOR_BOUNDARY_OK;
    plan->sandbox  = (struct spg_sandbox_spec){
         .enabled = true,
        /* Unconditional: the only way past the network check above is
         * uses_network == false, so an approved command has declared it does
         * not need the network -- and now the kernel holds it to that. */
         .allow_network = false,
        /* "/" as the writable bind is no sandbox at all: a command gated at the
         * filesystem root gets a private /tmp and nothing else writable. */
         .rw_dir = (request->working_dir != nullptr &&
                   strcmp(request->working_dir, "/") != 0)
                       ? request->working_dir
                       : nullptr,
    };
}

/* Is `dir` the allowed directory itself, or something below it?
 *
 * A plain strncmp was wrong twice over. It let a sibling through, because
 * "/scratch/ab" starts with "/scratch/a" while being an entirely different
 * directory; and it never resolved "..", so "/scratch/a/../.." walked out of
 * the sandbox while still matching the prefix. Both are bypasses of the one
 * boundary the executor has.
 *
 * So both sides are canonicalised and the match must end on a path separator.
 * realpath also resolves symlinks, which a lexical check cannot: a link inside
 * the allowed directory pointing anywhere else would otherwise pass.
 *
 * A path that does not resolve is denied. This check runs immediately before
 * execution (exec_command.c, shell_executor.c), never during replay, so a
 * working directory that cannot be resolved is one the child could not enter
 * either — refusing it costs nothing and keeps the gate fail-closed. */
static bool within_allowed_dir(const char *dir, const char *allowed) {
    if (dir == nullptr || allowed == nullptr || allowed[0] == '\0') {
        return false;
    }
    char real_dir[PATH_MAX];
    char real_allowed[PATH_MAX];
    if (realpath(dir, real_dir) == nullptr ||
        realpath(allowed, real_allowed) == nullptr) {
        return false;
    }
    const size_t allowed_n = strlen(real_allowed);
    if (strncmp(real_dir, real_allowed, allowed_n) != 0) {
        return false;
    }
    /* Equal, or the next character starts a new path segment. "/" is its own
     * case: it already ends in a separator, so nothing more is required. */
    return real_dir[allowed_n] == '\0' || real_dir[allowed_n] == '/' ||
           (allowed_n == 1u && real_allowed[0] == '/');
}

const char *spg_executor_boundary_reason_to_string(
    const enum spg_executor_boundary_reason reason) {
    switch (reason) {
    case SPG_EXECUTOR_BOUNDARY_OK:
        return "ok";
    case SPG_EXECUTOR_BOUNDARY_EXECUTION_DISABLED:
        return "execution_disabled";
    case SPG_EXECUTOR_BOUNDARY_POLICY_DENIED:
        return "policy_denied";
    case SPG_EXECUTOR_BOUNDARY_UNSUPPORTED_ACTION:
        return "unsupported_action";
    case SPG_EXECUTOR_BOUNDARY_NETWORK_FORBIDDEN:
        return "network_forbidden";
    case SPG_EXECUTOR_BOUNDARY_MISSING_COMMAND:
        return "missing_command";
    case SPG_EXECUTOR_BOUNDARY_BAD_WORKDIR:
        return "bad_workdir";
    case SPG_EXECUTOR_BOUNDARY_BAD_TIMEOUT:
        return "bad_timeout";
    case SPG_EXECUTOR_BOUNDARY_BAD_OUTPUT_LIMIT:
        return "bad_output_limit";
    case SPG_EXECUTOR_BOUNDARY_ENV_NOT_CLEARED:
        return "env_not_cleared";
    }
    return "unknown";
}

enum spg_status spg_executor_boundary_check(
    const struct spg_executor_boundary_config *config,
    const struct spg_recommendation *recommendation,
    const struct spg_policy_decision *policy_decision,
    const struct spg_executor_boundary_request *request,
    struct spg_executor_boundary_plan *plan) {
    if (config == nullptr || recommendation == nullptr ||
        policy_decision == nullptr || request == nullptr || plan == nullptr ||
        recommendation->state != SPG_RECOMMENDATION_VALID) {
        return SPG_E_INVALID_ARG;
    }
    deny(plan, SPG_EXECUTOR_BOUNDARY_EXECUTION_DISABLED);

    if (!config->execution_enabled) {
        return SPG_OK;
    }
    if (policy_decision->kind != SPG_POLICY_DECISION_ALLOW) {
        deny(plan, SPG_EXECUTOR_BOUNDARY_POLICY_DENIED);
        return SPG_OK;
    }
    if (recommendation->action_kind != SPG_ACTION_LOCAL_SHELL) {
        deny(plan, SPG_EXECUTOR_BOUNDARY_UNSUPPORTED_ACTION);
        return SPG_OK;
    }
    if (recommendation->action.uses_network) {
        deny(plan, SPG_EXECUTOR_BOUNDARY_NETWORK_FORBIDDEN);
        return SPG_OK;
    }
    if (!recommendation->has_command || recommendation->command.length == 0u) {
        deny(plan, SPG_EXECUTOR_BOUNDARY_MISSING_COMMAND);
        return SPG_OK;
    }
    if (!within_allowed_dir(request->working_dir,
                            config->allowed_workdir_prefix)) {
        deny(plan, SPG_EXECUTOR_BOUNDARY_BAD_WORKDIR);
        return SPG_OK;
    }
    if (request->timeout_ms == 0u ||
        request->timeout_ms > config->max_timeout_ms) {
        deny(plan, SPG_EXECUTOR_BOUNDARY_BAD_TIMEOUT);
        return SPG_OK;
    }
    if (request->stdout_limit_bytes == 0u ||
        request->stderr_limit_bytes == 0u ||
        request->stdout_limit_bytes > config->max_stdout_bytes ||
        request->stderr_limit_bytes > config->max_stderr_bytes) {
        deny(plan, SPG_EXECUTOR_BOUNDARY_BAD_OUTPUT_LIMIT);
        return SPG_OK;
    }
    if (config->require_clean_env && !request->env_cleared) {
        deny(plan, SPG_EXECUTOR_BOUNDARY_ENV_NOT_CLEARED);
        return SPG_OK;
    }

    allow(plan, request);
    return SPG_OK;
}

enum spg_status spg_executor_boundary_check_shell(
    const struct spg_executor_boundary_config *config, const char *command,
    const bool uses_network,
    const struct spg_executor_boundary_request *request,
    struct spg_executor_boundary_plan *plan) {
    const struct spg_recommendation rec = {
        .state       = SPG_RECOMMENDATION_VALID,
        .action_kind = SPG_ACTION_LOCAL_SHELL,
        .action      = {.uses_network = uses_network},
        .command     = {.offset = 0u,
                        .length = command != nullptr ? strlen(command) : 0u},
        .has_command = command != nullptr && command[0] != '\0',
    };
    const struct spg_policy_decision decision = {
        .kind = SPG_POLICY_DECISION_ALLOW,
    };
    return spg_executor_boundary_check(config, &rec, &decision, request, plan);
}
