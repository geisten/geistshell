#include "geistshell/eval.h"

#include "geistshell/journal.h"
#include "geistshell/policy.h"
#include "geistshell/recommendation.h"
#include "geistshell/sexpr.h"

#include <stdio.h>
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

enum { SPG_SHAPE_MAX_TOKENS = 16u, SPG_SHAPE_TOKEN_MAX = 48u };

enum spg_status spg_shape_from_script(const struct spg_fake_response responses[],
                                      const size_t n, const size_t cap,
                                      char out[], size_t *len) {
    if (responses == nullptr || out == nullptr || len == nullptr || cap == 0u) {
        return SPG_E_INVALID_ARG;
    }
    *len   = 0u;
    out[0] = '\0';

    /* Distinct "<kind>:<capability>" tokens, inserted in sorted order (small
     * fixed set — the codebase does no dynamic allocation). */
    char   tokens[SPG_SHAPE_MAX_TOKENS][SPG_SHAPE_TOKEN_MAX + 1u];
    size_t token_count = 0u;

    for (size_t i = 0u; i < n; i++) {
        struct spg_sexpr_token toks[256];
        struct spg_sexpr_node  nodes[256];
        struct spg_recommendation rec = {};
        struct spg_recommendation_error err = {};
        if (spg_recommendation_parse(responses[i].n, responses[i].text, 256u,
                                     toks, 256u, nodes, &rec, &err) != SPG_OK) {
            continue; /* best-effort: an unparseable reply adds no shape */
        }
        char token[SPG_SHAPE_TOKEN_MAX + 1u];
        const char *kind = spg_action_kind_to_string(rec.action_kind);
        if (rec.capability.length > 0u) {
            (void)snprintf(token, sizeof token, "%s:%.*s", kind,
                           (int)rec.capability.length,
                           responses[i].text + rec.capability.offset);
        } else {
            (void)snprintf(token, sizeof token, "%s", kind);
        }

        /* insert into the sorted set, skipping duplicates */
        size_t pos = 0u;
        bool   dup = false;
        while (pos < token_count) {
            const int cmp = strcmp(tokens[pos], token);
            if (cmp == 0) {
                dup = true;
                break;
            }
            if (cmp > 0) {
                break;
            }
            pos++;
        }
        if (dup || token_count == SPG_SHAPE_MAX_TOKENS) {
            continue;
        }
        for (size_t j = token_count; j > pos; j--) {
            memcpy(tokens[j], tokens[j - 1u], sizeof tokens[0]);
        }
        (void)snprintf(tokens[pos], sizeof tokens[0], "%s", token);
        token_count++;
    }

    size_t w = 0u;
    for (size_t i = 0u; i < token_count; i++) {
        const size_t tn = strlen(tokens[i]);
        const size_t sep = (i > 0u) ? 1u : 0u;
        if (w + sep + tn + 1u > cap) {
            out[w] = '\0';
            return SPG_E_LIMIT;
        }
        if (sep) {
            out[w++] = '+';
        }
        memcpy(out + w, tokens[i], tn);
        w += tn;
    }
    out[w] = '\0';
    *len   = w;
    return SPG_OK;
}
