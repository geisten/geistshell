#include "geistshell/agent_loop.h"

#include "geistshell/mem_store.h" /* P6: slug-triggered directive injection */

#include <stdio.h>
#include <string.h>

/* FNV-1a over a C string — enough to spot a repeated observation (#40) without a
 * second copy of the buffer; the heavy spg_hash is overkill here. */
static uint64_t obs_hash(const char *s) {
    uint64_t h = 1469598103934665603ull;
    for (; s != nullptr && *s != '\0'; s += 1) {
        h = (h ^ (uint64_t)(unsigned char)*s) * 1099511628211ull;
    }
    return h;
}

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
    uint64_t prev_obs_hash   = 0u;    /* #40: observation after the last */
    bool     have_prev_obs   = false; /* executed step, for stall detection */
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
                /* Slug-triggered auto-injection (P6): if a stored lesson names
                 * this failure slug, lead the repair observation with its
                 * earned directive — no memory_read needed, one slot, budgeted.
                 * The generic hint always follows so a repair works even when
                 * no lesson exists yet. */
                char   directive[SPG_MEM_DESC_MAX + 1u];
                size_t dn =
                    state->store != nullptr
                        ? spg_mem_directive(state->store, "lesson-rejected",
                                            config->lesson_budget_bytes,
                                            sizeof directive, directive)
                        : 0u;
                (void)snprintf(
                    workspace->observation_buf,
                    workspace->observation_capacity,
                    "%s%s[invalid recommendation: %s] Reply with exactly one "
                    "valid (recommend ...) form, or (recommend (kind finish) "
                    "(reason \"...\")).",
                    dn > 0u ? directive : "", dn > 0u ? " " : "",
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

        /* Convergence stop (#40): the step executed an allowed action but the
         * agent may never emit `finish`. Detect "no progress" and treat it as
         * completion so the run terminates FINISHED. A simulator action that did
         * not mutate the world (the executor picked a noop — nothing left to do)
         * is no progress; sim does not write the shared observation channel, so
         * it needs its own signal. For other actions, a repeated non-empty
         * observation is the signal. */
        if (config->finish_on_no_progress) {
            bool no_progress = false;
            if (step_result.recommendation.action_kind == SPG_ACTION_SIMULATOR) {
                no_progress = !step_result.sim.mutated;
            } else if (workspace->observation_buf != nullptr &&
                       workspace->observation_buf[0] != '\0') {
                const uint64_t h = obs_hash(workspace->observation_buf);
                no_progress       = have_prev_obs && h == prev_obs_hash;
                prev_obs_hash     = h;
                have_prev_obs     = true;
            }
            if (no_progress) {
                result->termination = SPG_AGENT_LOOP_FINISHED;
                return SPG_OK;
            }
        }
    }
    return SPG_OK;
}
