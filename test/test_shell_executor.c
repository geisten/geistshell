#include "geistshell/shell_executor.h"

#include <stdio.h>
#include <string.h>

static struct spg_shell_executor_config base_config(const bool enabled) {
    return (struct spg_shell_executor_config){
        .actor_id               = 1u,
        .timestamp_ns           = 1u,
        .parent_sequence        = 0u,
        .write_journal          = false,
        .execution_enabled      = enabled,
        .working_dir            = ".",
        .allowed_workdir_prefix = ".",
        .timeout_ms             = 5000u,
        .max_stdout_bytes       = 4096u,
        .max_stderr_bytes       = 1024u,
    };
}

static enum spg_status run(const char *cmd, const bool enabled,
                           struct spg_shell_executor_result *result,
                           char observation[static 1], size_t obs_cap) {
    struct spg_shell_executor_state state = {.journal = nullptr};
    const struct spg_shell_executor_config config = base_config(enabled);

    char payload[1024];
    char stdout_buf[4096];
    char stderr_buf[1024];
    const struct spg_shell_executor_workspace ws = {
        .payload_capacity     = sizeof payload,
        .payload              = payload,
        .observation_capacity = obs_cap,
        .observation          = observation,
        .stdout_capacity      = sizeof stdout_buf,
        .stdout_buf           = stdout_buf,
        .stderr_capacity      = sizeof stderr_buf,
        .stderr_buf           = stderr_buf,
    };

    const struct spg_recommendation rec = {
        .state       = SPG_RECOMMENDATION_VALID,
        .action_kind = SPG_ACTION_LOCAL_SHELL,
        .action      = {.kind = SPG_ACTION_LOCAL_SHELL, .uses_network = false},
        .command     = {.offset = 0u, .length = strlen(cmd)},
        .has_command = true,
    };
    const struct spg_policy_decision decision = {
        .kind = SPG_POLICY_DECISION_ALLOW,
    };
    return spg_shell_executor_step(&state, &config, strlen(cmd), cmd, &rec,
                                   &decision, &ws, result);
}

static int test_allow_runs_and_captures(void) {
    struct spg_shell_executor_result result = {};
    char observation[1024];
    const char cmd[] = "echo shell-exec-ok";
    if (run(cmd, true, &result, observation, sizeof observation) != SPG_OK) {
        return 1;
    }
    if (!result.approved || !result.started || !result.exited ||
        result.exit_code != 0) {
        return 1;
    }
    return (strstr(observation, "shell-exec-ok") != nullptr &&
            strstr(observation, "exit 0") != nullptr)
               ? 0
               : 1;
}

static int test_disabled_is_denied(void) {
    struct spg_shell_executor_result result = {};
    char observation[1024];
    const char cmd[] = "echo nope";
    if (run(cmd, false, &result, observation, sizeof observation) != SPG_OK) {
        return 1;
    }
    if (result.approved || result.started) {
        return 1;
    }
    return (result.boundary_reason ==
                SPG_EXECUTOR_BOUNDARY_EXECUTION_DISABLED &&
            strstr(observation, "denied") != nullptr)
               ? 0
               : 1;
}

static int test_nonzero_exit_captured(void) {
    struct spg_shell_executor_result result = {};
    char observation[1024];
    /* split_ws has no quoting; use a bare command that exits nonzero. */
    const char cmd[] = "false";
    if (run(cmd, true, &result, observation, sizeof observation) != SPG_OK) {
        return 1;
    }
    if (!result.approved || !result.started || !result.exited) {
        return 1;
    }
    return result.exit_code != 0 ? 0 : 1;
}

static int test_empty_command_errors(void) {
    struct spg_shell_executor_result result = {};
    char observation[1024];
    const char cmd[] = "   ";
    if (run(cmd, true, &result, observation, sizeof observation) != SPG_OK) {
        return 1;
    }
    return (!result.approved && result.run_status != SPG_OK &&
            strstr(observation, "error") != nullptr)
               ? 0
               : 1;
}

int main(void) {
    if (test_allow_runs_and_captures() != 0) {
        fprintf(stderr, "test_allow_runs_and_captures failed\n");
        return 1;
    }
    if (test_disabled_is_denied() != 0) {
        fprintf(stderr, "test_disabled_is_denied failed\n");
        return 1;
    }
    if (test_nonzero_exit_captured() != 0) {
        fprintf(stderr, "test_nonzero_exit_captured failed\n");
        return 1;
    }
    if (test_empty_command_errors() != 0) {
        fprintf(stderr, "test_empty_command_errors failed\n");
        return 1;
    }
    return 0;
}
