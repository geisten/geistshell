#include "geistshell/eval.h"

#include <string.h>

const char *spg_eval_outcome_to_string(const enum spg_eval_outcome o) {
    switch (o) {
    case SPG_EVAL_PASS:
        return "pass";
    case SPG_EVAL_FAIL_TERMINATION:
        return "fail_termination";
    case SPG_EVAL_FAIL_STEPS:
        return "fail_steps";
    case SPG_EVAL_FAIL_OBSERVATION:
        return "fail_observation";
    case SPG_EVAL_FAIL_RUN_ERROR:
        return "fail_run_error";
    }
    return "unknown";
}

enum spg_eval_outcome spg_eval_judge(const struct spg_eval_expect *expect,
                                     const struct spg_agent_loop_result *loop,
                                     const enum spg_status status,
                                     const char *observation) {
    if (status != SPG_OK) {
        return SPG_EVAL_FAIL_RUN_ERROR;
    }
    if (expect->check_termination && loop->termination != expect->termination) {
        return SPG_EVAL_FAIL_TERMINATION;
    }
    if ((expect->min_steps > 0u && loop->steps_taken < expect->min_steps) ||
        (expect->max_steps > 0u && loop->steps_taken > expect->max_steps)) {
        return SPG_EVAL_FAIL_STEPS;
    }
    if (expect->observation != nullptr &&
        strstr(observation, expect->observation) == nullptr) {
        return SPG_EVAL_FAIL_OBSERVATION;
    }
    return SPG_EVAL_PASS;
}

enum spg_status
spg_eval_run_case(const struct spg_fake_response *script, const size_t script_n,
                  const char *gate_marker,
                  const struct spg_agent_run_inputs *inputs,
                  const struct spg_agent_run_config *config,
                  const struct spg_agent_run_workspace *workspace,
                  const struct spg_eval_expect *expect,
                  struct spg_eval_case_result *result) {
    if (script == nullptr || script_n == 0u || inputs == nullptr ||
        config == nullptr || workspace == nullptr || expect == nullptr ||
        result == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *result = (struct spg_eval_case_result){};

    struct spg_model_adapter            model = {};
    const struct spg_model_adapter_config model_config = {
        .kind                = SPG_MODEL_ADAPTER_FAKE,
        .sampling            = {.top_p = 1.0f},
        .fake_response_count = script_n,
        .fake_responses      = script,
        .fake_gate_marker    = gate_marker,
    };
    if (spg_model_adapter_init(&model, &model_config) != SPG_OK) {
        return SPG_E_INVALID_ARG;
    }

    struct spg_agent_run_inputs run_inputs = *inputs;
    run_inputs.model                       = &model;

    struct spg_policy_usage      usage = {};
    struct spg_agent_loop_result loop  = {};
    const enum spg_status        status =
        spg_agent_run(&run_inputs, config, workspace, &usage, &loop);
    spg_model_adapter_destroy(&model);

    result->status       = status;
    result->termination  = loop.termination;
    result->steps_taken  = loop.steps_taken;
    result->repairs_used = loop.repairs_used;
    /* Concrete signal from the final tick for reflection to learn from. */
    result->reject_reason = loop.last.recommendation.reject_reason;
    result->deny_reason   = loop.last.policy_gate.decision.deny_reason;
    result->outcome =
        spg_eval_judge(expect, &loop, status, workspace->observation);
    return SPG_OK;
}
