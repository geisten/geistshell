/* nanosleep for the settle window: declared only under POSIX on glibc,
 * exposed by default on macOS — the difference cost a Pi round-trip. */
#define _POSIX_C_SOURCE 200809L

#include "geistshell/agent_loop.h"

#include <time.h>

#include "geistshell/mem_store.h" /* P6: slug-triggered directive injection */

#include <stdio.h>
#include <string.h>
#include <time.h>

/* Milliseconds on a clock that does not jump: a wall budget must not be
 * defeated by an NTP step or a timezone change. */
static uint64_t monotonic_ms(void) {
    struct timespec ts = {};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0u;
    }
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* FNV-1a over a C string — enough to spot a repeated observation (#40) without
 * a second copy of the buffer; the heavy spg_hash is overkill here. */
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

const char *
spg_agent_loop_termination_to_string(const enum spg_agent_loop_termination t) {
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
static void accumulate_usage(struct spg_policy_usage              *usage,
                             const struct spg_orchestrator_result *result) {
    add_u64(&usage->consumed.inference_steps, 1u);
    add_u64(&usage->consumed.tokens, (uint64_t)result->actor.tokens_decoded);
    if (spg_orchestrator_sim_executed(result)) {
        add_u64(&usage->consumed.sim_actions,
                result->recommendation.action.cost);
    }
    if (spg_orchestrator_memory_executed(result)) {
        add_u64(&usage->consumed.memory_actions,
                result->recommendation.action.cost);
    }
    if (spg_orchestrator_machine_executed(result)) {
        add_u64(&usage->consumed.machine_actions,
                result->recommendation.action.cost);
    }
    if (spg_orchestrator_shell_executed(result)) {
        add_u64(&usage->consumed.shell_actions,
                result->recommendation.action.cost);
    }
}

/* Newest journaled sequence from the step, for parent-threading the next one.
 */
static uint64_t step_sequence(const struct spg_orchestrator_result *result,
                              const uint64_t                        fallback) {
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

enum spg_status
spg_agent_loop_run(struct spg_orchestrator_state           *state,
                   const struct spg_agent_loop_config      *config,
                   const struct spg_orchestrator_workspace *workspace,
                   struct spg_policy_usage                 *usage,
                   struct spg_agent_loop_result            *result) {
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
    bool     made_progress   = false; /* an allowed action has executed */
    const uint64_t started_ms = monotonic_ms();
    for (size_t step = 0u; step < config->max_steps; step += 1u) {
        if ((config->token_budget > 0u &&
             usage->consumed.tokens >= config->token_budget) ||
            (config->step_budget > 0u &&
             usage->consumed.inference_steps >= config->step_budget) ||
            (config->wall_budget_ms > 0u &&
             monotonic_ms() - started_ms >= config->wall_budget_ms)) {
            result->termination = SPG_AGENT_LOOP_BUDGET;
            return SPG_OK;
        }
        /* Expose every event logged so far (steps 1..step-1) to this step. */
        if (feedback) {
            state->journal_header_count = state->journal->header_log_count;
        }

        struct spg_orchestrator_config step_config = config->base;
        step_config.timestamp_ns                   = (uint64_t)step + 1u;
        step_config.parent_sequence                = parent_sequence;

        struct spg_orchestrator_result step_result = {};
        const enum spg_status          status =
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

        /* #79: historise the snapshot THIS tick decided on, before any
         * refresh — the next tick then reads the trend up to and including
         * the state it is following, and the current-state block carries the
         * refreshed now. One push per tick, constant memory, no allocation. */
        if (state->machine_history != nullptr && state->machine != nullptr) {
            spg_machine_history_push(state->machine_history,
                                     (uint64_t)step + 1u, state->machine);
        }

        /* Latch the success marker while this step's observation is still the
         * current one — the next step overwrites the buffer. */
        if (config->observation_marker != nullptr && !result->observation_seen &&
            workspace->observation_buf != nullptr &&
            strstr(workspace->observation_buf, config->observation_marker) !=
                nullptr) {
            result->observation_seen = true;
        }

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
                    workspace->observation_buf, workspace->observation_capacity,
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
            /* #40 (Weg 2): a budget denial *after* the agent already made
             * progress means it has spent its allowance and is repeating a
             * completed action — converged, not failed. Terminate FINISHED so
             * an achieved goal (e.g. the command already ran, budget 1 spent)
             * still passes. A budget denial with no prior progress stays a real
             * DENIED. */
            const enum spg_policy_deny_reason dr =
                step_result.policy_gate.decision.deny_reason;
            if (config->finish_on_no_progress && made_progress &&
                (dr == SPG_POLICY_DENY_CAPABILITY_BUDGET ||
                 dr == SPG_POLICY_DENY_GLOBAL_BUDGET)) {
                result->termination = SPG_AGENT_LOOP_FINISHED;
                return SPG_OK;
            }
            result->termination = SPG_AGENT_LOOP_DENIED;
            return SPG_OK;
        }
        made_progress = true; /* reached only on an allowed, executed step */
        result->actions_executed += 1u;

        /* Phase 7: observe again, so the next tick reasons about the world the
         * action left behind rather than the one it decided on. This is the
         * whole difference between a sequence of decisions and a loop.
         *
         * Deliberately here and not at the top of the tick: refreshing before
         * anything happened would only cost a syscall and would make the first
         * decision race the sampler. */
        /* Two things can be re-observed: the host geistshell runs on, and the
         * plant it is driving. The plant does NOT ride on refresh_machine —
         * that flag means "re-read the host", and a plant run without host
         * telemetry would then observe the machine exactly once and steer
         * blind for the rest of the run. The presence of the readings buffer
         * is the intent. */
        const bool refresh_host =
            config->refresh_machine && state->machine != nullptr;
        const bool refresh_plant =
            state->device_state != nullptr && state->device != nullptr;
        /* Settle once for both, and only where something is actually being
         * measured: a scripted case describes the world after the action
         * directly and has nothing to wait for. The plant needs this more than
         * the host does — a heater read the instant after it was told to warm
         * up reports the temperature from before the command. */
        if (config->machine_settle_ms > 0u &&
            ((refresh_host && state->machine_after == nullptr) ||
             refresh_plant)) {
            const struct timespec settle = {
                .tv_sec  = (time_t)(config->machine_settle_ms / 1000u),
                .tv_nsec =
                    (long)((config->machine_settle_ms % 1000u) * 1000000u)};
            (void)nanosleep(&settle, nullptr);
        }
        if (refresh_host) {
            if (state->machine_after != nullptr) {
                /* Scripted: the world changes in the way the case describes. */
                *state->machine = *state->machine_after;
            } else {
                /* Live: re-read the host, feeding the previous counters back in
                 * so per-process CPU is a delta rather than unknown. */
                struct spg_machine_state next = {};
                if (spg_machine_sample_with_processes(
                        config->base.timestamp_ns, &state->machine->cpu,
                        state->machine->n_processes, state->machine->processes,
                        state->profile, &next) == SPG_OK) {
                    *state->machine = next;
                }
            }
        }

        /* A failed sample is not an error here: the channels that answered are
         * installed and the rest render `unknown`. One dead sensor must not
         * cost the agent its view of the whole plant. */
        if (refresh_plant) {
            (void)spg_device_sample(state->device, state->device_state);
        }

        /* Convergence stop (#40): the step executed an allowed action but the
         * agent may never emit `finish`. Detect "no progress" and treat it as
         * completion so the run terminates FINISHED. A simulator action that
         * did not mutate the world (the executor picked a noop — nothing left
         * to do) is no progress; sim does not write the shared observation
         * channel, so it needs its own signal. For other actions, a repeated
         * non-empty observation is the signal. */
        if (config->finish_on_no_progress) {
            bool no_progress = false;
            if (step_result.recommendation.action_kind ==
                SPG_ACTION_SIMULATOR) {
                no_progress = !step_result.sim.mutated;
            } else if (workspace->observation_buf != nullptr &&
                       workspace->observation_buf[0] != '\0') {
                const uint64_t h = obs_hash(workspace->observation_buf);
                no_progress      = have_prev_obs && h == prev_obs_hash;
                prev_obs_hash    = h;
                have_prev_obs    = true;
            }
            if (no_progress) {
                result->termination = SPG_AGENT_LOOP_FINISHED;
                return SPG_OK;
            }
        }
    }
    return SPG_OK;
}
