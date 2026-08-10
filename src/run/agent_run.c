#include "geistshell/agent_run.h"

#include "geistshell/graph.h"
#include "geistshell/mem_store.h" /* #40 follow-up: strong directive channel */
#include "geistshell/memory.h"
#include "geistshell/orchestrator.h"

static bool workspace_valid(const struct spg_agent_run_workspace *w) {
    return w != nullptr && w->context != nullptr && w->model_output != nullptr &&
           w->graph_refs != nullptr && w->memory_refs != nullptr &&
           w->journal_refs != nullptr && w->tokens != nullptr &&
           w->nodes != nullptr && w->policy_payload != nullptr &&
           w->sim_payload != nullptr && w->observation != nullptr;
}

enum spg_status spg_agent_run(const struct spg_agent_run_inputs *inputs,
                              const struct spg_agent_run_config *config,
                              const struct spg_agent_run_workspace *workspace,
                              struct spg_policy_usage *usage,
                              struct spg_agent_loop_result *result) {
    if (inputs == nullptr || config == nullptr || usage == nullptr ||
        result == nullptr || !workspace_valid(workspace) ||
        inputs->model == nullptr || inputs->policy == nullptr ||
        inputs->policy_text == nullptr || inputs->run == nullptr) {
        return SPG_E_INVALID_ARG;
    }

    struct spg_graph  graph  = {};
    struct spg_memory memory = {};
    spg_graph_init(&graph);
    spg_memory_init(&memory);

    workspace->observation[0] = '\0';

    /* Strong steering channel (#40 follow-up): render a designated lesson's
     * directive as (directive "...") every step, not buried in the index. Read
     * once; the state pointer is reused across the loop. */
    char        directive_buf[SPG_MEM_DESC_MAX + 1u];
    const char *directive = nullptr;
    if (config->directive_slug != nullptr && config->directive_slug[0] != '\0' &&
        inputs->store != nullptr &&
        spg_mem_directive(inputs->store, config->directive_slug, 0u,
                          sizeof directive_buf, directive_buf) > 0u) {
        directive = directive_buf;
    }

    const struct spg_orchestrator_workspace ow = {
        .actor = {.context_capacity      = workspace->context_capacity,
                  .context               = workspace->context,
                  .model_output_capacity = workspace->model_output_capacity,
                  .model_output          = workspace->model_output,
                  .graph_ref_capacity    = workspace->graph_ref_capacity,
                  .graph_refs            = workspace->graph_refs,
                  .memory_ref_capacity   = workspace->memory_ref_capacity,
                  .memory_refs           = workspace->memory_refs,
                  .journal_ref_capacity  = workspace->journal_ref_capacity,
                  .journal_refs          = workspace->journal_refs},
        .recommendation_token_capacity = workspace->token_capacity,
        .recommendation_tokens         = workspace->tokens,
        .recommendation_node_capacity  = workspace->node_capacity,
        .recommendation_nodes          = workspace->nodes,
        .policy_payload_capacity       = workspace->policy_payload_capacity,
        .policy_payload                = workspace->policy_payload,
        .sim_payload_capacity          = workspace->sim_payload_capacity,
        .sim_payload                   = workspace->sim_payload,
        .observation_capacity          = workspace->observation_capacity,
        .observation_buf               = workspace->observation,
        .shell_stdout_capacity         = workspace->shell_stdout_capacity,
        .shell_stdout_buf              = workspace->shell_stdout,
        .shell_stderr_capacity         = workspace->shell_stderr_capacity,
        .shell_stderr_buf              = workspace->shell_stderr,
        .memory_index_capacity         = workspace->memory_index_capacity,
        .memory_index_buf              = workspace->memory_index,
    };

    struct spg_orchestrator_state state = {
        .graph         = &graph,
        .memory        = &memory,
        .journal       = inputs->journal,
        .model         = inputs->model,
        .sim           = inputs->sim,
        .store         = inputs->store,
        .run           = inputs->run,
        .usage         = usage,
        .policy        = inputs->policy,
        .policy_text_n = inputs->policy_text_n,
        .policy_text   = inputs->policy_text,
        .observation   = workspace->observation,
        .exemplars     = inputs->exemplars,
        .goal          = inputs->goal,
        .directive     = directive,
        .machine       = inputs->machine,
    };

    const size_t refs = config->context_refs > 0u ? config->context_refs : 8u;
    const struct spg_agent_loop_config loop_config = {
        .base = {.actor_id          = 1u,
                 .context_limits    = {.graph_nodes    = refs,
                                       .memory_facts   = refs,
                                       .journal_events = refs},
                 .max_decode_tokens = 256u,
                 .reset_model_session = true,
                 .write_journal     = inputs->journal != nullptr,
                 .update_graph      = true,
                 .update_memory     = true,
                 .execution_enabled = config->execution_enabled,
                 .exec_working_dir  = config->exec_working_dir != nullptr
                                          ? config->exec_working_dir
                                          : ".",
                 .exec_workdir_prefix = config->exec_workdir_prefix != nullptr
                                            ? config->exec_workdir_prefix
                                            : ".",
                 .exec_timeout_ms   = config->exec_timeout_ms,
                 .exec_stdout_cap   = config->exec_stdout_cap,
                 .exec_stderr_cap   = config->exec_stderr_cap},
        .max_steps               = config->max_steps,
        .max_repairs             = config->max_repairs,
        .finish_on_no_progress   = config->finish_on_no_progress,
        .token_budget            = inputs->run->budgets.tokens,
        .step_budget             = inputs->run->budgets.inference_steps,
        .journal_header_capacity = workspace->trajectory_capacity,
        .journal_headers         = workspace->trajectory,
    };

    return spg_agent_loop_run(&state, &loop_config, &ow, usage, result);
}
