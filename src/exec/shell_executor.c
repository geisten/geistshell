#include "geistshell/shell_executor.h"

#include "geistshell/cmd_executor.h"
#include "geistshell/sexpr.h"

#include <stdio.h>
#include <string.h>

#define SHELL_CMDLINE_MAX 2048u

static bool workspace_valid(const struct spg_shell_executor_workspace *w) {
    return w != nullptr && w->payload != nullptr && w->payload_capacity > 0u &&
           w->observation != nullptr && w->observation_capacity > 0u &&
           w->stdout_buf != nullptr && w->stdout_capacity > 0u &&
           w->stderr_buf != nullptr && w->stderr_capacity > 0u;
}

/* Copy the command span out of text into cmd (NUL-terminated, bounded). */
static bool copy_command(size_t text_n, const char *text,
                         const struct spg_text_span span, char *cmd,
                         const size_t cap) {
    cmd[0] = '\0';
    if (!spg_sexpr_span_valid(text_n, span) || span.length + 1u > cap) {
        return false;
    }
    memcpy(cmd, text + span.offset, span.length);
    cmd[span.length] = '\0';
    return true;
}

/* Journal one (local_shell (command "...") (approved bool) (status ...)
 * [(exit N) (stdout_len N) (truncated bool)]) event. */
static enum spg_status journal_action(
    struct spg_shell_executor_state *state,
    const struct spg_shell_executor_config *config, const char *cmd,
    const struct spg_shell_executor_workspace *workspace,
    struct spg_shell_executor_result *result) {
    struct spg_sexpr_writer w;
    spg_sexpr_writer_init(&w, workspace->payload_capacity, workspace->payload);
    enum spg_status s = spg_sexpr_writer_append_text(&w, "(local_shell (command ");
    if (s == SPG_OK) {
        s = spg_sexpr_writer_append_text(&w, "\"");
    }
    /* cmd has no quotes/newlines of interest for the journal; emit raw bytes
     * (the executor already rejected anything unparseable). */
    if (s == SPG_OK) {
        s = spg_sexpr_writer_append_text(&w, cmd);
    }
    if (s == SPG_OK) {
        s = spg_sexpr_writer_append_text(&w, "\") (approved ");
    }
    if (s == SPG_OK) {
        s = spg_sexpr_writer_append_text(&w, result->approved ? "true" : "false");
    }
    if (s == SPG_OK && result->approved) {
        s = spg_sexpr_writer_append_text(&w, ") (exit ");
    }
    if (s == SPG_OK && result->approved) {
        s = spg_sexpr_writer_append_u64(&w, (uint64_t)(result->exit_code & 0xff));
    }
    if (s == SPG_OK && result->approved) {
        s = spg_sexpr_writer_append_text(&w, ") (stdout_len ");
    }
    if (s == SPG_OK && result->approved) {
        s = spg_sexpr_writer_append_size(&w, result->stdout_len);
    }
    if (s == SPG_OK) {
        s = spg_sexpr_writer_append_text(&w, result->approved ? "))" : ")");
    }
    result->payload_used      = w.used;
    result->payload_truncated = w.truncated;

    if (!config->write_journal || state->journal == nullptr) {
        return SPG_OK;
    }
    return spg_journal_writer_append(
        state->journal, config->timestamp_ns, config->parent_sequence,
        SPG_JOURNAL_EVENT_ACTION, result->run_status, w.used,
        (const uint8_t *)workspace->payload, &result->action_sequence);
}

enum spg_status spg_shell_executor_step(
    struct spg_shell_executor_state *state,
    const struct spg_shell_executor_config *config, const size_t text_n,
    const char text[], const struct spg_recommendation *recommendation,
    const struct spg_policy_decision *policy_decision,
    const struct spg_shell_executor_workspace *workspace,
    struct spg_shell_executor_result *result) {
    if (state == nullptr || config == nullptr || text == nullptr ||
        recommendation == nullptr || policy_decision == nullptr ||
        !workspace_valid(workspace) || result == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *result = (struct spg_shell_executor_result){.run_status = SPG_OK};
    workspace->observation[0] = '\0';

    if (policy_decision->kind != SPG_POLICY_DECISION_ALLOW ||
        recommendation->action_kind != SPG_ACTION_LOCAL_SHELL) {
        return SPG_E_INVALID_ARG;
    }

    char cmd[SHELL_CMDLINE_MAX];
    if (!copy_command(text_n, text, recommendation->command, cmd, sizeof cmd)) {
        result->run_status      = SPG_E_INVALID_ARG;
        result->boundary_reason = SPG_EXECUTOR_BOUNDARY_MISSING_COMMAND;
        (void)snprintf(workspace->observation, workspace->observation_capacity,
                       "exec error: command too long or invalid");
        result->observation_len = strlen(workspace->observation);
        return journal_action(state, config, cmd, workspace, result);
    }

    const char  *argv[SPG_CMD_MAX_ARGS];
    const size_t argc = spg_cmd_split_ws(cmd, SPG_CMD_MAX_ARGS, argv);
    if (argc == 0u) {
        result->run_status      = SPG_E_INVALID_ARG;
        result->boundary_reason = SPG_EXECUTOR_BOUNDARY_MISSING_COMMAND;
        (void)snprintf(workspace->observation, workspace->observation_capacity,
                       "exec error: empty command");
        result->observation_len = strlen(workspace->observation);
        return journal_action(state, config, cmd, workspace, result);
    }

    const struct spg_executor_boundary_config bcfg = {
        .execution_enabled      = config->execution_enabled,
        .allowed_workdir_prefix = config->allowed_workdir_prefix,
        .max_timeout_ms         = config->timeout_ms,
        .max_stdout_bytes       = config->max_stdout_bytes,
        .max_stderr_bytes       = config->max_stderr_bytes,
        .require_clean_env      = false,
    };
    const struct spg_executor_boundary_request breq = {
        .working_dir        = config->working_dir,
        .timeout_ms         = config->timeout_ms,
        .stdout_limit_bytes = config->max_stdout_bytes,
        .stderr_limit_bytes = config->max_stderr_bytes,
        .env_cleared        = false,
    };
    struct spg_executor_boundary_plan plan = {};
    if (spg_executor_boundary_check_shell(&bcfg, argv[0],
                                          recommendation->action.uses_network,
                                          &breq, &plan) != SPG_OK) {
        return SPG_E_INVALID_ARG;
    }
    result->boundary_reason = plan.reason;
    if (!plan.approved) {
        result->approved   = false;
        result->run_status = SPG_OK;
        (void)snprintf(workspace->observation, workspace->observation_capacity,
                       "exec denied: %s",
                       spg_executor_boundary_reason_to_string(plan.reason));
        result->observation_len = strlen(workspace->observation);
        return journal_action(state, config, cmd, workspace, result);
    }

    result->approved              = true;
    workspace->stdout_buf[0]      = '\0';
    workspace->stderr_buf[0]      = '\0';
    const struct spg_cmd_request creq = {
        .argc        = argc,
        .argv        = argv,
        .working_dir = config->working_dir,
        .timeout_ms  = config->timeout_ms,
        .clear_env   = false,
        .limits      = SPG_CMD_DEFAULT_LIMITS,
        .stdout_cap  = workspace->stdout_capacity,
        .stdout_buf  = workspace->stdout_buf,
        .stderr_cap  = workspace->stderr_capacity,
        .stderr_buf  = workspace->stderr_buf,
    };
    struct spg_cmd_result cres = {};
    (void)spg_cmd_executor_run(1u, &creq, &cres);
    result->run_status       = cres.status;
    result->started          = cres.started;
    result->exited           = cres.exited;
    result->exit_code        = cres.exit_code;
    result->timed_out        = cres.timed_out;
    result->stdout_len       = cres.stdout_len;
    result->stdout_truncated = cres.stdout_truncated;

    if (!cres.started) {
        (void)snprintf(workspace->observation, workspace->observation_capacity,
                       "exec error: cannot run %s", argv[0]);
    } else {
        (void)snprintf(workspace->observation, workspace->observation_capacity,
                       "exit %d%s%s%s%s", cres.exit_code,
                       workspace->stdout_buf[0] != '\0' ? "\n" : "",
                       workspace->stdout_buf,
                       workspace->stderr_buf[0] != '\0' ? "\n[stderr] " : "",
                       workspace->stderr_buf);
    }
    result->observation_len = strlen(workspace->observation);
    return journal_action(state, config, cmd, workspace, result);
}
