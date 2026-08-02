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
    SPG_EVAL_FAIL_OBSERVATION, /* required observation substring absent */
    SPG_EVAL_FAIL_RUN_ERROR,   /* the run returned a non-OK status */
};

struct spg_eval_expect {
    bool                            check_termination;
    enum spg_agent_loop_termination termination;
    size_t                          min_steps;   /* 0 = no lower bound */
    size_t                          max_steps;   /* 0 = no upper bound */
    const char                     *observation; /* required substring, or null */
};

struct spg_eval_case_result {
    enum spg_eval_outcome           outcome;
    enum spg_agent_loop_termination termination; /* actual */
    size_t                          steps_taken;
    size_t                          repairs_used;
    enum spg_status                 status; /* run status (SPG_OK unless error) */
    /* Concrete failure signal from the final tick, so reflection can distil a
     * lesson from what actually went wrong rather than the failure mode alone.
     * Meaningful for the matching termination (reject_reason when rejected,
     * deny_reason when denied); otherwise the NONE/zero value. */
    enum spg_recommendation_reject_reason reject_reason;
    enum spg_policy_deny_reason           deny_reason;
};

[[nodiscard]] const char *spg_eval_outcome_to_string(enum spg_eval_outcome o);

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

#ifdef __cplusplus
}
#endif

#endif
