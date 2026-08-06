#ifndef GEISTSHELL_MODEL_ADAPTER_H
#define GEISTSHELL_MODEL_ADAPTER_H

#include "geistshell/policy.h" /* enum spg_action_kind (constrained decode caps) */
#include "geistshell/status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct geist_backend;
struct geist_model;
struct geist_session;

/* A policy capability the constrained decoder may put in the capability slot,
 * tagged with the action kind it applies to (memory capabilities are expanded
 * to one entry per memory_* kind by the caller). name is borrowed and must be
 * null-terminated. #34. */
struct spg_model_capability {
    const char          *name;
    enum spg_action_kind kind;
};

enum spg_model_adapter_kind {
    SPG_MODEL_ADAPTER_FAKE = 0,
    SPG_MODEL_ADAPTER_GEIST,
    /* Drive a strong external model over an OpenAI-compatible HTTP endpoint.
     * Only functional when built with SPG_ENABLE_REMOTE (libcurl); otherwise
     * init/generate return SPG_E_UNSUPPORTED. */
    SPG_MODEL_ADAPTER_REMOTE,
};

struct spg_model_sampling {
    size_t   max_seq_len;
    float    temperature;
    float    top_p;
    int      top_k;
    uint64_t random_seed;
};

/* One canned fake-model reply (a recommendation text). */
struct spg_fake_response {
    size_t      n;
    const char *text;
};

struct spg_model_adapter_config {
    enum spg_model_adapter_kind kind;

    const char *backend_name;
    const char *model_path;
    const char *awq_scales_path;

    /* REMOTE: OpenAI-compatible endpoint URL, model name, and bearer key. All
     * borrowed — they must outlive the adapter. api_key may be null for
     * key-less local gateways. */
    const char *endpoint_url;
    const char *model_name;
    const char *api_key;

    struct spg_model_sampling sampling;

    /* Single canned reply (returned on every generate). */
    size_t      fake_response_n;
    const char *fake_response;

    /* Scripted replies: the i-th generate returns fake_responses[i]; once
     * exhausted the fake model "stops" (empty output, stopped_by_eos). Takes
     * precedence over the single fake_response when count > 0. The array must
     * outlive the adapter. */
    size_t                          fake_response_count;
    const struct spg_fake_response *fake_responses;

    /* Evaluation aid: while this marker is absent from the prompt, the fake
     * emits an invalid form (the loop rejects it); once present, the script
     * runs. Null disables gating. */
    const char *fake_gate_marker;

    /* Constrained decoding (geistshell#34, first cut): a literal the GEIST
     * model's output is FORCED to begin with, then it decodes freely. Forcing
     * the recommendation opening (e.g. "(recommend (kind ") past the hardest
     * part lifts a small model over the structure it cannot reliably produce
     * from a grammar description alone. Null = free decode (default). GEIST
     * only; ignored by fake/remote. */
    const char *force_prefix;

    /* Capabilities the constrained decoder may emit in the capability slot,
     * masked per chosen kind. Borrowed; must outlive the adapter. Empty → the
     * capability slot free-decodes (valid form, but may be policy-denied). */
    const struct spg_model_capability *capabilities;
    size_t                             capability_count;
};

struct spg_model_adapter {
    enum spg_model_adapter_kind kind;
    bool                        initialized;

    struct geist_backend *backend;
    struct geist_model   *model;
    struct geist_session *session;

    size_t      fake_response_n;
    const char *fake_response;

    size_t                          fake_response_count;
    const struct spg_fake_response *fake_responses;
    size_t                          fake_index; /* next scripted reply */
    const char                     *fake_gate_marker;
    const char                     *force_prefix; /* constrained decode (#34) */
    const struct spg_model_capability *capabilities; /* #34 capability mask */
    size_t                             capability_count;

    /* REMOTE transport state: opaque CURL handle, borrowed config strings, and
     * the sampling values forwarded to the chat/completions request. */
    void       *http;
    const char *endpoint_url;
    const char *model_name;
    const char *api_key;
    float       temperature;
    float       top_p;

    /* Best-of-N (#… verifier-guided sampling): when temperature > 0 the masked
     * choice slots (kind, capability) sample among the valid tokens instead of
     * taking the argmax, so different seeds explore different valid decisions
     * and the verifier can pick the winning run. Own RNG because the mask can't
     * go through the session sampler. GEIST only. */
    uint64_t    choice_rng;
};

struct spg_model_generate_request {
    size_t      prompt_n;
    const char *prompt;
    bool        reset_session;
    size_t      max_decode_tokens;
};

struct spg_model_generate_result {
    size_t output_capacity;
    char  *output;

    size_t output_used;
    size_t tokens_decoded;
    bool   output_truncated;
    bool   stopped_by_token_limit;
    /* The model emitted an end-of-sequence token (decoding stopped on its own
     * before reaching max_decode_tokens), as opposed to being cut off by the
     * token budget. Mutually exclusive with stopped_by_token_limit. */
    bool   stopped_by_eos;
};

[[nodiscard]] enum spg_status
spg_model_adapter_init(struct spg_model_adapter *adapter,
                       const struct spg_model_adapter_config *config);

void spg_model_adapter_destroy(struct spg_model_adapter *adapter);

[[nodiscard]] enum spg_status
spg_model_generate(struct spg_model_adapter *adapter,
                   const struct spg_model_generate_request *request,
                   struct spg_model_generate_result *result);

#ifdef __cplusplus
}
#endif

#endif
