#ifndef GEISTSHELL_MACHINE_GOAL_H
#define GEISTSHELL_MACHINE_GOAL_H

#include "geistshell/machine_state.h"
#include "geistshell/sexpr.h"
#include "geistshell/status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What a machine run is for, and how to tell whether it worked (roadmap phase
 * 9, #69).
 *
 *   (machine-goal
 *     (max-temperature-mc 70000)
 *     (min-critical-service-health-bp 9500)
 *     (max-actions 3))
 *
 * Four things are kept apart on purpose, because collapsing them is how a
 * safety property quietly becomes a suggestion:
 *
 *   1. HARD CONSTRAINTS — the numbers above. Measurable against the final
 *      snapshot, and the only thing that decides whether a run succeeded.
 *   2. OPTIMISATION PREFERENCES — none yet. The slot is named because the
 *      separation matters, not because something fills it. An earlier
 *      prefer-min-energy field was parsed, rendered into the context and never
 *      judged, which made the run promise a trade-off nobody evaluated.
 *   3. SEMANTIC PROCESS RESTRICTIONS — what may be paused or stopped. Those
 *      live in the process profile (#62) and are enforced by the policy gate.
 *   4. POLICY AND SAFETY — capabilities and budgets, in the policy config.
 *
 * A goal can never widen 2, 3 or 4. Asking for something the policy forbids
 * does not make it allowed; it makes the goal IMPOSSIBLE, which is a terminal
 * state and not an error. The direction only goes one way: a goal can ask for
 * less than the policy permits, never for more. */

/* Absent constraint. Distinct from 0, which is a real and very strict bound. */
#define SPG_GOAL_UNSET UINT64_MAX
#define SPG_GOAL_UNSET_S INT64_MIN

struct spg_machine_goal {
    /* Hard constraints. */
    int64_t  max_temperature_mc;
    uint64_t min_critical_health_bp;
    uint64_t max_actions;

    bool present; /* false = no goal was configured */
};

/* Why a run did or did not satisfy its goal. A bare pass/fail cannot tell a
 * harness which knob to turn, and "it said finish" is not a reason. */
enum spg_goal_verdict {
    SPG_GOAL_SATISFIED = 0,
    SPG_GOAL_TEMPERATURE_TOO_HIGH,
    SPG_GOAL_CRITICAL_UNHEALTHY,
    SPG_GOAL_TOO_MANY_ACTIONS,
    /* A constraint exists but the value it names could not be measured.
     * Deliberately a failure, never a pass: an unreadable sensor is not
     * evidence that a limit was respected. */
    SPG_GOAL_UNMEASURABLE,
    /* No goal was configured, so there is nothing to satisfy. Reported rather
     * than silently passing. */
    SPG_GOAL_NO_GOAL,
};

struct spg_goal_evaluation {
    enum spg_goal_verdict verdict;
    /* The measured value that decided it, for the record. */
    int64_t  temperature_mc;
    uint64_t critical_health_bp;
    uint64_t actions_used;
};

[[nodiscard]] enum spg_status
spg_machine_goal_load(size_t input_n, const char input[], size_t token_capacity,
                      struct spg_sexpr_token   tokens[static token_capacity],
                      size_t                   node_capacity,
                      struct spg_sexpr_node    nodes[static node_capacity],
                      struct spg_machine_goal *out);

/* Judge a finished run against its goal, from the OBSERVED state.
 *
 * This is the whole point of the phase: a run is not successful because the
 * model emitted `finish`. actions_used is what the run actually executed, so a
 * goal that bounds actions is checked against the journal's truth rather than
 * the model's account of it. */
[[nodiscard]] enum spg_status
spg_machine_goal_evaluate(const struct spg_machine_goal  *goal,
                          const struct spg_machine_state *final_state,
                          uint64_t                        actions_used,
                          struct spg_goal_evaluation     *out);

/* Deterministic (machine-goal ...) block for the model context. Same shape as
 * the file, so what the model reads is what the harness checks. */
[[nodiscard]] enum spg_status
spg_machine_goal_render(const struct spg_machine_goal *goal,
                        size_t dst_capacity, char dst[static dst_capacity],
                        size_t *out_required);

[[nodiscard]] const char *
spg_goal_verdict_to_string(enum spg_goal_verdict verdict);

#ifdef __cplusplus
}
#endif

#endif
