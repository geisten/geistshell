#ifndef GEISTSHELL_EVAL_H
#define GEISTSHELL_EVAL_H

#include "geistshell/agent_run.h"
#include "geistshell/model_adapter.h"
#include "geistshell/policy.h"
#include "geistshell/recommendation.h"
#include "geistshell/status.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Deterministic evaluation of a governed agent against scripted scenarios. Each
 * case drives a fake-model script through spg_agent_run and checks the outcome
 * against expectations, yielding a structured per-case result. The result is
 * machine-consumable (not just pass/fail): a production self-improvement loop
 * reads the termination reason, step/repair counts, and failure mode to decide
 * what to adjust, then re-evaluates against this same baseline. */

enum spg_eval_outcome {
    SPG_EVAL_PASS = 0,
    SPG_EVAL_FAIL_TERMINATION, /* termination reason did not match */
    SPG_EVAL_FAIL_STEPS,       /* step count outside [min, max] */
    SPG_EVAL_FAIL_OBSERVATION, /* required substring in no step's observation */
    SPG_EVAL_FAIL_RUN_ERROR,   /* the run returned a non-OK status */
};

struct spg_eval_expect {
    bool                            check_termination;
    enum spg_agent_loop_termination termination;
    size_t                          min_steps;   /* 0 = no lower bound */
    size_t                          max_steps;   /* 0 = no upper bound */
    /* Required substring, or null. Satisfied if it appeared in ANY step's
     * observation, provided the caller also set the same string as
     * spg_agent_run_config.observation_marker so the loop could latch it;
     * otherwise only the final observation is checked. */
    const char                     *observation;
};

struct spg_eval_case_result {
    enum spg_eval_outcome           outcome;
    enum spg_agent_loop_termination termination; /* actual */
    size_t                          steps_taken;
    /* Actions the run executed, for a goal that bounds them (phase 9). */
    size_t                          actions_executed;
    size_t                          repairs_used;
    enum spg_status                 status; /* run status (SPG_OK unless error) */
    /* Concrete failure signal from the final tick, so reflection can distil a
     * lesson from what actually went wrong rather than the failure mode alone.
     * Meaningful for the matching termination (reject_reason when rejected,
     * deny_reason when denied); otherwise the NONE/zero value. */
    enum spg_recommendation_reject_reason reject_reason;
    enum spg_policy_deny_reason           deny_reason;
    /* Tokens the run consumed (usage.consumed.tokens), so a budget-bound run
     * is diagnosable from the per-case verdict alone (#126). Deterministic for
     * scripted fakes — the fake decoder counts one token per tick. */
    uint64_t tokens_consumed;
};

[[nodiscard]] const char *spg_eval_outcome_to_string(enum spg_eval_outcome o);

/* Answer-free run quality, for selecting among N sampled attempts when nobody
 * knows the right answer (geistshell#55).
 *
 * `--best-of N` used to select with spg_eval_judge, which needs a declared
 * (expect ...) — i.e. the answer. Without one the feature silently collapsed to
 * a single attempt, so in production, where nobody has the expected
 * observation, it was off. This is the selector that needs no oracle: the
 * information is already in every run's termination, and nothing read it.
 *
 * Higher is better. The order follows the parse/gate/task ladder (#53), NOT the
 * ordering originally written into #55 — which put `denied` above `max_steps`
 * and is wrong: a denied run parsed but never got past the policy gate, while a
 * max_steps run parsed, WAS allowed, and executed real actions. It got strictly
 * further; it simply ran out of room.
 *
 *   4  finished   reached a terminal state on its own
 *   3  max_steps  acted validly, ran out of steps
 *   2  budget     acted validly, ran out of budget
 *   1  denied     produced a valid action the gate refused
 *   0  rejected   never produced a valid form
 *  -1  error      the harness failed; not a measurement of the model at all
 *
 * A non-OK run status is always -1 regardless of termination. */
[[nodiscard]] int spg_run_rank(enum spg_status                 status,
                               enum spg_agent_loop_termination termination);

/* Judge a finished run against expectations (termination, step bounds, an
 * observation substring). Exposed so callers that drive the run themselves —
 * e.g. a real-model task case via spg_agent_run — score it the same way. */
[[nodiscard]] enum spg_eval_outcome
spg_eval_judge(const struct spg_eval_expect *expect,
               const struct spg_agent_loop_result *loop, enum spg_status status,
               const char *observation);

/* Build a scripted fake model from script[0..script_n), run one governed agent
 * case via spg_agent_run against inputs/config (inputs->model is ignored and
 * replaced by the scripted fake), and check it against expect. The observation
 * substring is matched against workspace->observation after the run. Returns
 * SPG_E_INVALID_ARG on bad arguments; otherwise SPG_OK with the verdict in
 * *result (a failed expectation is a result, not a return error). */
/* gate_marker (nullable) gates the scripted fake: until it appears in the
 * prompt the agent's replies are rejected — letting a deterministic eval show a
 * recalled lesson flipping a case from failing to passing. */
[[nodiscard]] enum spg_status
spg_eval_run_case(const struct spg_fake_response *script, size_t script_n,
                  const char *gate_marker,
                  const struct spg_agent_run_inputs *inputs,
                  const struct spg_agent_run_config *config,
                  const struct spg_agent_run_workspace *workspace,
                  const struct spg_eval_expect *expect,
                  struct spg_eval_case_result *result);

/* Reconstruct a fake-model script from a run's journal (docs/LEARNING.md P3):
 * each MODEL_OUTPUT event becomes one spg_fake_response, in sequence order.
 * The response texts are copied into text_buf; each response's `text` points
 * into it. Replaying the returned script through spg_eval_run_case against the
 * same inputs reproduces the run deterministically (the journal is
 * byte-replayable), so a real run becomes a frozen case the mint gate can
 * re-run without live inference.
 *
 * Fills responses[0..*count) and returns SPG_OK. SPG_E_INVALID_ARG on null
 * args; SPG_E_JOURNAL_CORRUPT / SPG_E_IO on a read failure; SPG_E_LIMIT if the
 * outputs exceed max_responses or text_cap. */
[[nodiscard]] enum spg_status spg_eval_script_from_journal(
    const char *journal_path, size_t max_responses,
    struct spg_fake_response responses[], size_t text_cap, char text_buf[],
    size_t *count);

/* Canonical task-shape key from a reconstructed script (docs/LEARNING.md P4):
 * the SET of "<action_kind>:<capability>" tokens the run's recommendations use
 * (a finish action has no capability, so just "<action_kind>"), deduplicated,
 * sorted, joined with '+'. command/target specifics are ignored, so two runs
 * that touch the same capabilities share a shape — the key the positive-guard
 * ring dedups on and the token spg_reflect_outcome keys its slug by.
 *
 * Writes a NUL-terminated key into out[0..cap); *len receives its length.
 * Unparseable responses are skipped (a shape is best-effort, not a gate).
 * Returns SPG_E_INVALID_ARG on null args, SPG_E_LIMIT if the key exceeds cap. */
[[nodiscard]] enum spg_status spg_shape_from_script(
    const struct spg_fake_response responses[], size_t n, size_t cap,
    char out[], size_t *len);

/* #12 (LEARNING.md decision 6): how the shape key is built.
 *
 * SET is the default and stays what spg_shape_from_script computes: the
 * deduplicated, sorted capability set — cheaper and more bounded. SEQUENCE is
 * the finer key for when too many distinct scripts share one set and a single
 * guard stops protecting meaningfully different tasks: the ORDERED sequence
 * of "<kind>:<capability>" tokens, joined with '>', consecutive duplicates
 * collapsed (a retried step is the same step, not a new shape). `finish` is
 * excluded in both modes. A trajectory longer than SPG_SHAPE_MAX_TOKENS
 * truncates deterministically at that many tokens. */
enum spg_shape_mode {
    SPG_SHAPE_MODE_SET = 0,
    SPG_SHAPE_MODE_SEQUENCE,
};

[[nodiscard]] enum spg_status spg_shape_from_script_mode(
    const struct spg_fake_response responses[], size_t n,
    enum spg_shape_mode mode, size_t cap, char out[], size_t *len);

/* Per-slug failure recurrence in one journal (docs/LEARNING.md P7): the
 * benefit of a kept lesson is not proven at mint time but observed in the
 * field — a lesson that works makes its failure slug stop recurring. This
 * tallies the loop-failure events a real run journals: a rejected
 * recommendation (ERROR + "recommendation_rejected") and a policy denial
 * (POLICY_DECISION with a deny_reason other than NONE). The counts ACCUMULATE
 * into *out, so summing across a run's journals is repeated calls. Model-free:
 * reads the journal only. Returns SPG_E_INVALID_ARG on null args, a journal
 * read error otherwise; a missing/short journal counts nothing and is OK. */
struct spg_recurrence {
    size_t rejected; /* slug lesson-rejected */
    size_t denied;   /* slug lesson-denied */
};

[[nodiscard]] enum spg_status spg_journal_recurrence(
    const char *journal_path, struct spg_recurrence *out);

#ifdef __cplusplus
}
#endif

#endif
