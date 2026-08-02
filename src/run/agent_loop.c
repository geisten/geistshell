#include "geistshell/agent_loop.h"

#include <stdio.h>

static void add_u64(uint64_t *acc, const uint64_t v) {
    if (*acc > UINT64_MAX - v) {
        *acc = UINT64_MAX;
    } else {
        *acc += v;
    }
}

const char *spg_agent_loop_termination_to_string(
    const enum spg_agent_loop_termination t) {
    switch (t) {
    case SPG_AGENT_LOOP_FINISHED:
        return "finished";
    case SPG_AGENT_LOOP_MAX_STEPS:
        return "max_steps";
    case SPG_AGENT_LOOP_REJECTED:
        return "rejected";
    case SPG_AGENT_LOOP_DENIED:
        return "denied";
    case SPG_AGENT_LOOP_BUDGET:
        return "budget";
    case SPG_AGENT_LOOP_ERROR:
        return "error";
    }
    return "unknown";
}

/* Charge the step's consumption against the running usage. */
static void accumulate_usage(struct spg_policy_usage *usage,
                             const struct spg_orchestrator_result *result) {
    add_u64(&usage->consumed.inference_steps, 1u);
    add_u64(&usage->consumed.tokens, (uint64_t)result->actor.tokens_decoded);
    if (spg_orchestrator_sim_executed(result)) {
        add_u64(&usage->consumed.sim_actions, result->recommendation.action.cost);
    }
    if (spg_orchestrator_memory_executed(result)) {
        add_u64(&usage->consumed.memory_actions,
                result->recommendation.action.cost);
    }
    if (spg_orchestrator_shell_executed(result)) {
        add_u64(&usage->consumed.shell_actions,
                result->recommendation.action.cost);
    }
}

/* Newest journaled sequence from the step, for parent-threading the next one. */
static uint64_t step_sequence(const struct spg_orchestrator_result *result,
                              const uint64_t fallback) {
    if (result->shell.action_sequence != 0u) {
        return result->shell.action_sequence;
    }
    if (result->memory.memory_sequence != 0u) {
        return result->memory.memory_sequence;
    }
    if (result->sim.memory_sequence != 0u) {
        return result->sim.memory_sequence;
    }
    if (result->sim.graph_sequence != 0u) {
        return result->sim.graph_sequence;
    }
    if (result->sim.sim_sequence != 0u) {
        return result->sim.sim_sequence;
    }
    if (result->policy_gate.policy_sequence != 0u) {
        return result->policy_gate.policy_sequence;
    }
    if (result->actor.model_output_sequence != 0u) {
        return result->actor.model_output_sequence;
    }
    if (result->actor.model_input_sequence != 0u) {
        return result->actor.model_input_sequence;
    }
    return fallback;
}

enum spg_status spg_agent_loop_run(
    struct spg_orchestrator_state *state,
    const struct spg_agent_loop_config *config,
    const struct spg_orchestrator_workspace *workspace,
    struct spg_policy_usage *usage, struct spg_agent_loop_result *result) {
    if (state == nullptr || config == nullptr || workspace == nullptr ||
        usage == nullptr || result == nullptr || config->max_steps == 0u) {
        return SPG_E_INVALID_ARG;
    }
    *result = (struct spg_agent_loop_result){
        .termination = SPG_AGENT_LOOP_MAX_STEPS,
    };
    state->usage = usage;

    /* Trajectory feedback: bind the journal writer's header log to the caller
     * array so each step's events become visible to the next step's context. */
    const bool feedback = config->journal_headers != nullptr &&
                          config->journal_header_capacity > 0u &&
                          state->journal != nullptr;
    if (feedback) {
        spg_journal_writer_set_header_log(state->journal,
                                          config->journal_header_capacity,
                                          config->journal_headers);
        state->journal_headers = config->journal_headers;
    }

    uint64_t parent_sequence = config->base.parent_sequence;
    for (size_t step = 0u; step < config->max_steps; step += 1u) {
        if ((config->token_budget > 0u &&
             usage->consumed.tokens >= config->token_budget) ||
            (config->step_budget > 0u &&
             usage->consumed.inference_steps >= config->step_budget)) {
            result->termination = SPG_AGENT_LOOP_BUDGET;
            return SPG_OK;
        }
        /* Expose every event logged so far (steps 1..step-1) to this step. */
        if (feedback) {
            state->journal_header_count = state->journal->header_log_count;
        }

        struct spg_orchestrator_config step_config = config->base;
        step_config.timestamp_ns    = (uint64_t)step + 1u;
        step_config.parent_sequence = parent_sequence;

        struct spg_orchestrator_result step_result = {};
        const enum spg_status status =
            spg_orchestrator_tick(state, &step_config, workspace, &step_result);
        result->steps_taken = step + 1u;
        result->last        = step_result;
        result->last_status = status;
        if (status != SPG_OK) {
            result->termination = SPG_AGENT_LOOP_ERROR;
            return status;
        }

        accumulate_usage(usage, &step_result);
        parent_sequence = step_sequence(&step_result, parent_sequence);

        if (spg_orchestrator_finished(&step_result)) {
            result->termination = SPG_AGENT_LOOP_FINISHED;
            return SPG_OK;
        }
        if (step_result.stage ==
            SPG_ORCHESTRATOR_STAGE_RECOMMENDATION_REJECTED) {
            /* Self-repair: surface the parse error as the next observation and
             * retry, rather than giving up on one malformed reply. */
            if (result->repairs_used < config->max_repairs &&
                workspace->observation_buf != nullptr &&
                workspace->observation_capacity > 0u) {
                (void)snprintf(
                    workspace->observation_buf,
                    workspace->observation_capacity,
                    "[invalid recommendation: %s] Reply with exactly one valid "
                    "(recommend ...) form, or (recommend (kind finish) "
                    "(reason \"...\")).",
                    spg_recommendation_reject_reason_to_string(
                        step_result.recommendation.reject_reason));
                result->repairs_used += 1u;
                continue;
            }
            result->termination = SPG_AGENT_LOOP_REJECTED;
            return SPG_OK;
        }
        if (spg_orchestrator_policy_evaluated(&step_result) &&
            step_result.policy_gate.decision.kind !=
                SPG_POLICY_DECISION_ALLOW) {
            result->termination = SPG_AGENT_LOOP_DENIED;
            return SPG_OK;
        }
    }
    return SPG_OK;
}
