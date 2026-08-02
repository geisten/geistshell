#include "geistshell/model_adapter.h"

#include <geist.h>

#include <math.h>
#include <string.h>

#ifdef SPG_ENABLE_REMOTE
#include "geistshell/model_remote.h"
#endif

static enum spg_status map_geist_status(const enum geist_status status) {
    switch (status) {
    case GEIST_OK:
        return SPG_OK;
    case GEIST_E_OOM:
        return SPG_E_OOM;
    case GEIST_E_INVALID_ARG:
        return SPG_E_INVALID_ARG;
    case GEIST_E_INTERNAL:
    case GEIST_E_BACKEND:
        return SPG_E_MODEL;
    case GEIST_E_FILE_NOT_FOUND:
        return SPG_E_NOT_FOUND;
    case GEIST_E_IO:
        return SPG_E_IO;
    case GEIST_E_FORMAT:
        return SPG_E_FORMAT;
    case GEIST_E_UNSUPPORTED:
        return SPG_E_UNSUPPORTED;
    case GEIST_E_NOT_FOUND:
        return SPG_E_NOT_FOUND;
    case GEIST_E_INVALID_STATE:
        return SPG_E_INVALID_STATE;
    case GEIST_E_TOO_MANY_TOKENS:
        return SPG_E_BUDGET_EXCEEDED;
    }
    return SPG_E_MODEL;
}

static bool adapter_kind_valid(const enum spg_model_adapter_kind kind) {
    return kind == SPG_MODEL_ADAPTER_FAKE || kind == SPG_MODEL_ADAPTER_GEIST ||
           kind == SPG_MODEL_ADAPTER_REMOTE;
}

static bool sampling_valid(const struct spg_model_sampling *sampling) {
    if (sampling == nullptr) {
        return false;
    }
    return isfinite(sampling->temperature) && sampling->temperature >= 0.0f &&
           isfinite(sampling->top_p) && sampling->top_p >= 0.0f &&
           sampling->top_p <= 1.0f && sampling->top_k >= 0;
}

static bool prompt_valid(const struct spg_model_generate_request *request) {
    return request != nullptr && request->prompt != nullptr &&
           request->prompt[request->prompt_n] == '\0';
}

static bool result_valid(const struct spg_model_generate_result *result) {
    return result != nullptr && result->output != nullptr &&
           result->output_capacity > 0u;
}

static void reset_result(struct spg_model_generate_result *result) {
    result->output_used            = 0u;
    result->tokens_decoded         = 0u;
    result->output_truncated       = false;
    result->stopped_by_token_limit = false;
    result->stopped_by_eos         = false;
    result->output[0]              = '\0';
}

static enum spg_status append_bytes(struct spg_model_generate_result *result,
                                    const size_t n,
                                    const char bytes[static n]) {
    if (n == 0u) {
        return SPG_OK;
    }
    if (result->output_used >= result->output_capacity ||
        n > result->output_capacity - result->output_used - 1u) {
        const size_t available =
            result->output_used < result->output_capacity
                ? result->output_capacity - result->output_used - 1u
                : 0u;
        if (available > 0u) {
            memcpy(result->output + result->output_used, bytes, available);
            result->output_used += available;
        }
        result->output[result->output_used] = '\0';
        result->output_truncated            = true;
        return SPG_E_LIMIT;
    }
    memcpy(result->output + result->output_used, bytes, n);
    result->output_used += n;
    result->output[result->output_used] = '\0';
    return SPG_OK;
}

static enum spg_status generate_fake(struct spg_model_adapter *adapter,
                                     const struct spg_model_generate_request *request,
                                     struct spg_model_generate_result *result) {
    /* Prompt-gated fake (evaluation harness): until the gate marker appears in
     * the prompt, emit an invalid form so the loop rejects it. The marker is a
     * lesson the agent must have recalled — letting a deterministic eval show
     * that learning a lesson flips a failing case to passing. */
    if (adapter->fake_gate_marker != nullptr &&
        adapter->fake_gate_marker[0] != '\0' &&
        (request->prompt == nullptr ||
         strstr(request->prompt, adapter->fake_gate_marker) == nullptr)) {
        result->tokens_decoded = 1u;
        return append_bytes(result, 9u, "(blocked)");
    }
    const char *resp = adapter->fake_response;
    size_t      n    = adapter->fake_response_n;
    if (adapter->fake_responses != nullptr) {
        if (adapter->fake_index >= adapter->fake_response_count) {
            result->stopped_by_eos = true; /* script exhausted -> model stops */
            return SPG_OK;
        }
        resp = adapter->fake_responses[adapter->fake_index].text;
        n    = adapter->fake_responses[adapter->fake_index].n;
        adapter->fake_index += 1u;
    }
    if (n == 0u) {
        result->stopped_by_token_limit = true;
        return SPG_OK;
    }
    result->tokens_decoded = 1u;
    return append_bytes(result, n, resp);
}

static enum spg_status generate_geist(
    struct spg_model_adapter *adapter,
    const struct spg_model_generate_request *request,
    struct spg_model_generate_result *result) {
    if (request->reset_session) {
        const enum geist_status reset_status =
            geist_session_reset(adapter->session);
        if (reset_status != GEIST_OK) {
            return map_geist_status(reset_status);
        }
    }

    enum geist_status status =
        geist_session_set_prompt(adapter->session, request->prompt);
    if (status != GEIST_OK) {
        return map_geist_status(status);
    }

    for (size_t i = 0u; i < request->max_decode_tokens; i += 1u) {
        geist_token_t token = 0;
        status = geist_session_decode_step(adapter->session, &token);
        if (status != GEIST_OK) {
            return map_geist_status(status);
        }
        result->tokens_decoded += 1u;

        const char *piece = geist_session_token_to_str(adapter->session, token);
        if (piece == nullptr || piece[0] == '\0') {
            result->stopped_by_eos = true;
            break;
        }
        const enum spg_status append_status =
            append_bytes(result, strlen(piece), piece);
        if (append_status != SPG_OK) {
            return append_status;
        }
    }
    result->stopped_by_token_limit =
        !result->stopped_by_eos &&
        result->tokens_decoded >= request->max_decode_tokens;
    return SPG_OK;
}

enum spg_status
spg_model_adapter_init(struct spg_model_adapter *adapter,
                       const struct spg_model_adapter_config *config) {
    if (adapter == nullptr || config == nullptr ||
        !adapter_kind_valid(config->kind) || !sampling_valid(&config->sampling)) {
        return SPG_E_INVALID_ARG;
    }
    *adapter = (struct spg_model_adapter){
        .kind = config->kind,
    };

    if (config->kind == SPG_MODEL_ADAPTER_FAKE) {
        if (config->fake_response_n > 0u && config->fake_response == nullptr) {
            return SPG_E_INVALID_ARG;
        }
        if (config->fake_response_count > 0u &&
            config->fake_responses == nullptr) {
            return SPG_E_INVALID_ARG;
        }
        adapter->fake_response_n     = config->fake_response_n;
        adapter->fake_response       = config->fake_response;
        adapter->fake_response_count = config->fake_response_count;
        adapter->fake_responses      = config->fake_responses;
        adapter->fake_index          = 0u;
        adapter->fake_gate_marker    = config->fake_gate_marker;
        adapter->initialized         = true;
        return SPG_OK;
    }

    if (config->kind == SPG_MODEL_ADAPTER_REMOTE) {
#ifdef SPG_ENABLE_REMOTE
        return spg_remote_init(adapter, config);
#else
        return SPG_E_UNSUPPORTED;
#endif
    }

    if (config->model_path == nullptr || config->model_path[0] == '\0') {
        return SPG_E_INVALID_ARG;
    }

    struct geist_backend_opts backend_opts = {
        .max_concurrent_sessions = 1,
    };
    enum geist_status geist_status = geist_backend_create(
        config->backend_name == nullptr ? "auto" : config->backend_name,
        &backend_opts, nullptr, &adapter->backend);
    if (geist_status != GEIST_OK) {
        spg_model_adapter_destroy(adapter);
        return map_geist_status(geist_status);
    }

    geist_status =
        geist_model_load(config->model_path, adapter->backend, &adapter->model);
    if (geist_status != GEIST_OK) {
        spg_model_adapter_destroy(adapter);
        return map_geist_status(geist_status);
    }

    const struct geist_session_opts session_opts = {
        .max_seq_len     = config->sampling.max_seq_len,
        .temperature     = config->sampling.temperature,
        .top_p           = config->sampling.top_p,
        .top_k           = config->sampling.top_k,
        .random_seed     = config->sampling.random_seed,
        .awq_scales_path = config->awq_scales_path,
    };
    geist_status = geist_session_create(adapter->model, adapter->backend,
                                        &session_opts, &adapter->session);
    if (geist_status != GEIST_OK) {
        spg_model_adapter_destroy(adapter);
        return map_geist_status(geist_status);
    }

    adapter->initialized = true;
    return SPG_OK;
}

void spg_model_adapter_destroy(struct spg_model_adapter *adapter) {
    if (adapter == nullptr) {
        return;
    }
#ifdef SPG_ENABLE_REMOTE
    if (adapter->kind == SPG_MODEL_ADAPTER_REMOTE) {
        spg_remote_destroy(adapter);
    }
#endif
    if (adapter->session != nullptr) {
        geist_session_destroy(adapter->session);
    }
    if (adapter->model != nullptr) {
        geist_model_destroy(adapter->model);
    }
    if (adapter->backend != nullptr) {
        geist_backend_destroy(adapter->backend);
    }
    *adapter = (struct spg_model_adapter){};
}

enum spg_status
spg_model_generate(struct spg_model_adapter *adapter,
                   const struct spg_model_generate_request *request,
                   struct spg_model_generate_result *result) {
    if (adapter == nullptr || !adapter->initialized || !prompt_valid(request) ||
        !result_valid(result)) {
        return SPG_E_INVALID_ARG;
    }
    reset_result(result);
    if (request->max_decode_tokens == 0u) {
        result->stopped_by_token_limit = true;
        return SPG_OK;
    }

    switch (adapter->kind) {
    case SPG_MODEL_ADAPTER_FAKE:
        return generate_fake(adapter, request, result);
    case SPG_MODEL_ADAPTER_GEIST:
        if (adapter->session == nullptr) {
            return SPG_E_INVALID_STATE;
        }
        return generate_geist(adapter, request, result);
    case SPG_MODEL_ADAPTER_REMOTE:
#ifdef SPG_ENABLE_REMOTE
        return spg_remote_generate(adapter, request, result);
#else
        return SPG_E_UNSUPPORTED;
#endif
    }
    return SPG_E_INVALID_STATE;
}
