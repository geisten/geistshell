#include "geistshell/executor_boundary.h"

#include <stdio.h>
#include <string.h>

static struct spg_recommendation local_shell_recommendation(void) {
    return (struct spg_recommendation){
        .state       = SPG_RECOMMENDATION_VALID,
        .action_kind = SPG_ACTION_LOCAL_SHELL,
        .action      = {.kind         = SPG_ACTION_LOCAL_SHELL,
                        .uses_network = false,
                        .cost         = 1u},
        .command     = {.offset = 0u, .length = strlen("make test")},
        .has_command = true,
    };
}

static struct spg_policy_decision allow_policy(void) {
    return (struct spg_policy_decision){
        .kind        = SPG_POLICY_DECISION_ALLOW,
        .deny_reason = SPG_POLICY_DENY_NONE,
    };
}

static struct spg_executor_boundary_config boundary_config(void) {
    return (struct spg_executor_boundary_config){
        .execution_enabled      = true,
        .allowed_workdir_prefix = "/tmp/geistshell",
        .max_timeout_ms         = 1000u,
        .max_stdout_bytes       = 4096u,
        .max_stderr_bytes       = 4096u,
        .require_clean_env      = true,
    };
}

static struct spg_executor_boundary_request boundary_request(void) {
    return (struct spg_executor_boundary_request){
        .working_dir        = "/tmp/geistshell/lab",
        .timeout_ms         = 250u,
        .stdout_limit_bytes = 1024u,
        .stderr_limit_bytes = 1024u,
        .env_cleared        = true,
    };
}

static int test_default_denies_execution(void) {
    struct spg_recommendation rec = local_shell_recommendation();
    struct spg_policy_decision policy = allow_policy();
    struct spg_executor_boundary_config config = boundary_config();
    struct spg_executor_boundary_request request = boundary_request();
    struct spg_executor_boundary_plan plan = {};
    config.execution_enabled = false;

    if (spg_executor_boundary_check(&config, &rec, &policy, &request, &plan) !=
        SPG_OK) {
        return 1;
    }
    return !plan.approved &&
                   plan.reason == SPG_EXECUTOR_BOUNDARY_EXECUTION_DISABLED
               ? 0
               : 1;
}

static int test_allows_when_explicitly_enabled_and_bounded(void) {
    struct spg_recommendation rec = local_shell_recommendation();
    struct spg_policy_decision policy = allow_policy();
    struct spg_executor_boundary_config config = boundary_config();
    struct spg_executor_boundary_request request = boundary_request();
    struct spg_executor_boundary_plan plan = {};

    if (spg_executor_boundary_check(&config, &rec, &policy, &request, &plan) !=
        SPG_OK) {
        return 1;
    }
    return plan.approved && plan.reason == SPG_EXECUTOR_BOUNDARY_OK ? 0 : 1;
}

static int test_rejects_bad_workdir(void) {
    struct spg_recommendation rec = local_shell_recommendation();
    struct spg_policy_decision policy = allow_policy();
    struct spg_executor_boundary_config config = boundary_config();
    struct spg_executor_boundary_request request = boundary_request();
    struct spg_executor_boundary_plan plan = {};
    request.working_dir = "/etc";

    if (spg_executor_boundary_check(&config, &rec, &policy, &request, &plan) !=
        SPG_OK) {
        return 1;
    }
    return !plan.approved && plan.reason == SPG_EXECUTOR_BOUNDARY_BAD_WORKDIR
               ? 0
               : 1;
}

static int test_invalid_args(void) {
    struct spg_recommendation rec = local_shell_recommendation();
    struct spg_policy_decision policy = allow_policy();
    struct spg_executor_boundary_request request = boundary_request();
    struct spg_executor_boundary_plan plan = {};
    if (spg_executor_boundary_check(nullptr, &rec, &policy, &request, &plan) !=
        SPG_E_INVALID_ARG) {
        return 1;
    }
    return spg_executor_boundary_reason_to_string(
               SPG_EXECUTOR_BOUNDARY_BAD_TIMEOUT) != nullptr
               ? 0
               : 1;
}

int main(void) {
    if (test_default_denies_execution() != 0) {
        fprintf(stderr, "test_default_denies_execution failed\n");
        return 1;
    }
    if (test_allows_when_explicitly_enabled_and_bounded() != 0) {
        fprintf(stderr,
                "test_allows_when_explicitly_enabled_and_bounded failed\n");
        return 1;
    }
    if (test_rejects_bad_workdir() != 0) {
        fprintf(stderr, "test_rejects_bad_workdir failed\n");
        return 1;
    }
    if (test_invalid_args() != 0) {
        fprintf(stderr, "test_invalid_args failed\n");
        return 1;
    }
    return 0;
}
