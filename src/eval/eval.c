#include "geistshell/eval.h"

#include "geistshell/journal.h"

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

enum spg_status spg_eval_script_from_journal(
    const char *journal_path, const size_t max_responses,
    struct spg_fake_response responses[], const size_t text_cap,
    char text_buf[], size_t *count) {
    if (journal_path == nullptr || responses == nullptr ||
        text_buf == nullptr || count == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *count = 0u;

    struct spg_journal_reader reader;
    enum spg_status status = spg_journal_reader_open(&reader, journal_path);
    if (status != SPG_OK) {
        return status;
    }

    /* A recommendation is small; the scratch only needs to hold one MODEL_OUTPUT
     * payload. Larger records (the MODEL_INPUT context) overflow it and are
     * skipped — we do not need them. */
    char            scratch[8192];
    size_t          used   = 0u; /* bytes of text_buf consumed */
    size_t          n      = 0u; /* fake responses collected */
    enum spg_status result = SPG_OK;
    for (;;) {
        struct spg_journal_record record = {};
        const enum spg_status rs = spg_journal_reader_next(
            &reader, sizeof scratch, (uint8_t *)scratch, &record);
        if (rs == SPG_E_NOT_FOUND) {
            break; /* end of journal */
        }
        if (rs == SPG_E_LIMIT) {
            /* Payload larger than the scratch. A MODEL_OUTPUT that big cannot
             * be reconstructed; any other kind we skip. */
            if (record.header.event_kind == SPG_JOURNAL_EVENT_MODEL_OUTPUT) {
                result = SPG_E_LIMIT;
                break;
            }
            continue;
        }
        if (rs != SPG_OK) {
            result = rs;
            break;
        }
        if (record.header.event_kind != SPG_JOURNAL_EVENT_MODEL_OUTPUT) {
            continue;
        }
        if (n >= max_responses || used + record.payload_used > text_cap) {
            result = SPG_E_LIMIT;
            break;
        }
        memcpy(text_buf + used, scratch, record.payload_used);
        responses[n].n    = record.payload_used;
        responses[n].text = text_buf + used;
        used += record.payload_used;
        n += 1u;
    }

    (void)spg_journal_reader_close(&reader);
    if (result == SPG_OK) {
        *count = n;
    }
    return result;
}
