#include "geistshell/executor_boundary.h"

#include <string.h>

static void deny(struct spg_executor_boundary_plan *plan,
                 const enum spg_executor_boundary_reason reason) {
    plan->approved = false;
    plan->reason   = reason;
}

static void allow(struct spg_executor_boundary_plan *plan) {
    plan->approved = true;
    plan->reason   = SPG_EXECUTOR_BOUNDARY_OK;
}

static bool has_prefix(const char *text, const char *prefix) {
    if (text == nullptr || prefix == nullptr || prefix[0] == '\0') {
        return false;
    }
    const size_t prefix_n = strlen(prefix);
    return strncmp(text, prefix, prefix_n) == 0;
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
    if (!has_prefix(request->working_dir, config->allowed_workdir_prefix)) {
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

    allow(plan);
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
