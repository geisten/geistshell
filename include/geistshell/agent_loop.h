#ifndef GEISTSHELL_AGENT_LOOP_H
#define GEISTSHELL_AGENT_LOOP_H

#include "geistshell/orchestrator.h"
#include "geistshell/policy.h"
#include "geistshell/status.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Governed multi-step agent loop. Each step is one policy-gated orchestrator
 * tick; the step's tool result is journaled and (via the workspace observation
 * buffer) fed into the next step's context. The loop runs until the agent emits
 * `finish` or a termination condition is reached. The orchestrator tick stays
 * the deterministic single-step primitive; this driver only adds iteration,
 * usage accounting, and termination. */

enum spg_agent_loop_termination {
    SPG_AGENT_LOOP_FINISHED = 0, /* agent emitted `finish`, or (with
                                    finish_on_no_progress) converged: an executed
                                    step left the observation unchanged */
    SPG_AGENT_LOOP_MAX_STEPS,    /* hit the step cap */
    SPG_AGENT_LOOP_REJECTED,     /* a step's recommendation was malformed */
    SPG_AGENT_LOOP_DENIED,       /* a step was denied by policy */
    SPG_AGENT_LOOP_BUDGET,       /* token budget exhausted */
    SPG_AGENT_LOOP_ERROR,        /* a tick returned a non-OK status */
};

struct spg_agent_loop_config {
    /* Per-step orchestrator config. timestamp_ns and parent_sequence are
     * overwritten each step (logical step index / threaded sequence). */
    struct spg_orchestrator_config base;
    size_t                         max_steps;
    uint64_t                       token_budget; /* 0 = unlimited */
    /* Policy step budget: the loop stops once this many steps have run (each
     * step consumes one inference_step). Sourced from budgets.inference_steps,
     * so loop length is policy-controlled, not only bounded by max_steps.
     * 0 = unlimited. */
    uint64_t step_budget;

    /* Self-repair: when a step's recommendation is malformed, write the parse
     * error into the observation channel and retry instead of terminating, up
     * to this many times total. 0 = terminate on the first rejection. Repairs
     * still count against max_steps. Requires an observation buffer in the
     * workspace (observation_buf). */
    size_t max_repairs;

    /* Convergence stop (geistshell#40): treat an executed step that leaves the
     * observation byte-identical to the previous executed step's as completion
     * (FINISHED) — the agent is repeating itself with no effect on the world.
     * Closes the gap where a constrained model emits valid, executing actions
     * but never chooses `(kind finish)`, so it acts forever until the step cap.
     * Off by default (a plain max_steps run is unchanged); the CLI enables it. */
    bool finish_on_no_progress;

    /* Slug-triggered lesson auto-injection budget (docs/LEARNING.md P6): when a
     * step is rejected and a stored lesson names that failure slug, its
     * one-line directive is injected into the repair observation without the
     * model having to recall it — capped to this many bytes (0 = the store's
     * description cap; a small model with a short sequence length sets it low).
     * At most one directive per tick. Requires state->store. */
    size_t lesson_budget_bytes;

    /* Optional trajectory feedback: a caller-owned array the loop binds to the
     * journal writer so every event from earlier steps is rendered into the
     * next step's context (the agent sees its own history, not just the last
     * observation). Null disables it. Requires base.write_journal + a journal. */
    size_t                            journal_header_capacity;
    struct spg_journal_record_header *journal_headers;
};

struct spg_agent_loop_result {
    size_t                          steps_taken;
    size_t                          repairs_used; /* self-repair retries spent */
    enum spg_agent_loop_termination termination;
    enum spg_status                 last_status; /* last tick status */
    struct spg_orchestrator_result  last;        /* final step's result */
};

[[nodiscard]] const char *
spg_agent_loop_termination_to_string(enum spg_agent_loop_termination t);

/* Runs the loop. usage accumulates across steps and must be the same object
 * state->usage points at (the driver re-points it to be safe). Returns the last
 * tick's status: SPG_OK for every termination except SPG_AGENT_LOOP_ERROR,
 * which returns the failing tick's status. */
[[nodiscard]] enum spg_status spg_agent_loop_run(
    struct spg_orchestrator_state *state,
    const struct spg_agent_loop_config *config,
    const struct spg_orchestrator_workspace *workspace,
    struct spg_policy_usage *usage, struct spg_agent_loop_result *result);

#ifdef __cplusplus
}
#endif

#endif
