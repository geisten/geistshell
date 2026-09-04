#include "geistshell/model_adapter.h"

#include "geistshell/grammar_mask.h"  /* #34: kind + capability masks */
#include "geistshell/policy_config.h" /* SPG_POLICY_MAX_CAPABILITIES */

#include <geist.h>
#include <geist_util.h> /* #34: tokenize + prefill_tokens + peek_logits */

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

/* Emit a scaffold literal (#34): append its exact bytes to the output and feed
 * its tokens into the KV so the model's next prediction is grounded on it. */
static enum spg_status emit_literal(struct spg_model_adapter *adapter,
                                    struct spg_model_generate_result *result,
                                    const char *text) {
    geist_token_t     ids[256];
    size_t            n      = 0u;
    enum geist_status status = geist_session_tokenize(
        adapter->session, text, sizeof ids / sizeof ids[0], ids, &n);
    if (status != GEIST_OK) {
        return map_geist_status(status);
    }
    const enum spg_status as = append_bytes(result, strlen(text), text);
    if (as != SPG_OK) {
        return as;
    }
    if (n > 0u) {
        status = geist_session_prefill_tokens(adapter->session, n, ids);
        if (status != GEIST_OK) {
            return map_geist_status(status);
        }
        result->tokens_decoded += n;
    }
    return SPG_OK;
}

/* #125 reason-first: decode up to `budget` free tokens (the model's own
 * thinking) into the KV cache, then emit them as ONE parse-safe comment line
 * before the forced prefix. The tokens condition every constrained choice that
 * follows; the emitted comment is skipped by the s-expr parser, so the form it
 * then reads is unchanged. budget 0 is a no-op. */
static enum spg_status
emit_reason_prefix(struct spg_model_adapter         *adapter,
                   struct spg_model_generate_result *result,
                   const size_t                      budget) {
    if (budget == 0u) {
        return SPG_OK;
    }
    char   raw[512];
    size_t used = 0u;
    for (size_t j = 0u; j < budget && used + 1u < sizeof raw; j += 1u) {
        geist_token_t     token  = 0;
        enum geist_status status = geist_session_decode_step(adapter->session,
                                                             &token);
        if (status != GEIST_OK) {
            return map_geist_status(status);
        }
        result->tokens_decoded += 1u;
        const char *piece = geist_session_token_to_str(adapter->session, token);
        if (piece == nullptr || piece[0] == '\0') {
            break; /* eos ends the reasoning */
        }
        for (const char *p = piece; *p != '\0' && used + 1u < sizeof raw;
             p += 1u) {
            raw[used++] = *p;
        }
    }
    raw[used] = '\0';
    if (used == 0u) {
        return SPG_OK; /* the model volunteered nothing */
    }
    char         comment[600];
    const size_t n = spg_reason_comment(raw, sizeof comment, comment);
    return n > 0u ? append_bytes(result, n, comment) : SPG_OK;
}

/* Free-decode one scaffold string value (#34): the model fills the slot, but we
 * stop at the first character that would close or corrupt the STRING — the
 * closing quote or a newline (a newline is a hard error inside an s-expr string;
 * see sexpr.c). Parens are NOT stops: inside a quoted string they are literal,
 * so the model may emit commands like `awk '{print}'` or `grep (x)`. The
 * surrounding scaffold literal supplies the closing quote. Bounded so a runaway
 * slot cannot eat the budget. */
static enum spg_status decode_string_slot(struct spg_model_adapter *adapter,
                                          struct spg_model_generate_result *result) {
    for (size_t j = 0u; j < 64u; j += 1u) {
        geist_token_t     token  = 0;
        enum geist_status status = geist_session_decode_step(adapter->session, &token);
        if (status != GEIST_OK) {
            return map_geist_status(status);
        }
        result->tokens_decoded += 1u;
        const char *piece = geist_session_token_to_str(adapter->session, token);
        if (piece == nullptr || piece[0] == '\0') {
            return SPG_OK; /* eos ends the slot */
        }
        const size_t stop = strcspn(piece, "\"\n\r");
        if (piece[stop] != '\0') {
            return append_bytes(result, stop, piece); /* delimiter reached */
        }
        const enum spg_status as = append_bytes(result, strlen(piece), piece);
        if (as != SPG_OK) {
            return as;
        }
    }
    return SPG_OK; /* length cap */
}

/* Decode one bare integer. Stops at the first byte that cannot belong to one,
 * so the scaffold's own ")" terminates the slot; a minus is accepted only as
 * the very first byte. Capped at 12 bytes, which is wider than any 16-bit
 * register value and narrow enough that a model looping on digits cannot run
 * the slot away. */
static enum spg_status
decode_number_slot(struct spg_model_adapter         *adapter,
                   struct spg_model_generate_result *result) {
    size_t emitted = 0u;
    for (size_t j = 0u; j < 12u; j += 1u) {
        geist_token_t     token  = 0;
        enum geist_status status = geist_session_decode_step(adapter->session, &token);
        if (status != GEIST_OK) {
            return map_geist_status(status);
        }
        result->tokens_decoded += 1u;
        const char *piece = geist_session_token_to_str(adapter->session, token);
        if (piece == nullptr || piece[0] == '\0') {
            return SPG_OK;
        }
        size_t take = 0u;
        while (piece[take] != '\0') {
            const char c = piece[take];
            const bool digit = c >= '0' && c <= '9';
            const bool sign  = c == '-' && emitted == 0u && take == 0u;
            if (!digit && !sign) {
                break;
            }
            take += 1u;
        }
        if (take > 0u) {
            const enum spg_status as = append_bytes(result, take, piece);
            if (as != SPG_OK) {
                return as;
            }
            emitted += take;
        }
        if (piece[take] != '\0') {
            /* A non-numeric byte ends the slot. If nothing was emitted the
             * value is empty and the recommendation parser rejects it — which
             * is the right outcome: a repair pass can retry, but a slot that
             * invented a 0 would put a number on the wire that no model chose.
             */
            return SPG_OK;
        }
    }
    return SPG_OK;
}

/* Decode one slot constrained to a fixed vocabulary (#34): greedily keep only
 * tokens that leave the emitted text a live prefix of some candidate name,
 * until one is complete. Leading detok whitespace is stripped from the appended
 * text so the value carries no spurious space (correct for kind and capability,
 * whose names contain none). The chosen name is written to out[]. Bails (leaving
 * out partial) if no valid continuation exists. */
/* xorshift64 -> uniform [0,1). The choice-slot best-of-N sampler needs its own
 * RNG because the mask cannot go through the session's sampler. */
static double choice_rand(struct spg_model_adapter *adapter) {
    uint64_t x = adapter->choice_rng ? adapter->choice_rng : 0x9e3779b97f4a7c15ull;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    adapter->choice_rng = x;
    return (double) (x >> 11) * (1.0 / 9007199254740992.0);
}

static enum spg_status decode_choice_slot(
    struct spg_model_adapter *adapter, struct spg_model_generate_result *result,
    const char *const *names, const size_t names_n, char *out,
    const size_t out_cap) {
    size_t used = 0u;
    out[0]      = '\0';
    for (size_t step = 0u;
         step < 24u && !spg_choice_complete(names, names_n, out); step += 1u) {
        size_t       n_vocab = 0u;
        const float *logits =
            geist_session_peek_logits(adapter->session, &n_vocab);
        if (logits == nullptr || n_vocab == 0u) {
            break;
        }
        /* Collect the valid tokens once, then pick: argmax (greedy) or a
         * softmax draw at temperature (best-of-N explores valid decisions).
         * ponytail: 512-candidate cap — kind/capability prefixes never approach
         * it; widen if a larger vocabulary of names is ever masked here. */
        geist_token_t cand[512];
        float         cand_logit[512];
        size_t        nc   = 0u;
        float         maxl = -INFINITY;
        for (size_t t = 0u; t < n_vocab && nc < 512u; t += 1u) {
            const char *piece =
                geist_session_token_to_str(adapter->session, (geist_token_t) t);
            if (piece == nullptr ||
                !spg_choice_prefix_ok(names, names_n, out, piece)) {
                continue;
            }
            cand[nc]       = (geist_token_t) t;
            cand_logit[nc] = logits[t];
            if (logits[t] > maxl) {
                maxl = logits[t];
            }
            nc += 1u;
        }
        if (nc == 0u) {
            break;
        }
        geist_token_t best = cand[0];
        if (adapter->temperature > 0.0f) {
            double sum = 0.0;
            for (size_t i = 0u; i < nc; i += 1u) {
                sum += exp((double) (cand_logit[i] - maxl) /
                           (double) adapter->temperature);
            }
            const double r   = choice_rand(adapter) * sum;
            double       acc = 0.0;
            best             = cand[nc - 1u]; /* fp-rounding fallback */
            for (size_t i = 0u; i < nc; i += 1u) {
                acc += exp((double) (cand_logit[i] - maxl) /
                           (double) adapter->temperature);
                if (r <= acc) {
                    best = cand[i];
                    break;
                }
            }
        } else {
            float best_logit = cand_logit[0];
            for (size_t i = 1u; i < nc; i += 1u) {
                if (cand_logit[i] > best_logit) {
                    best_logit = cand_logit[i];
                    best       = cand[i];
                }
            }
        }
        const char *piece = geist_session_token_to_str(adapter->session, best);
        const char *ap    = piece;
        while (*ap == ' ' || *ap == '\t') {
            ap += 1;
        }
        const size_t pl = strlen(ap);
        if (used + pl >= out_cap) {
            break;
        }
        if (pl > 0u) {
            const enum spg_status as = append_bytes(result, pl, ap);
            if (as != SPG_OK) {
                return as;
            }
            memcpy(out + used, ap, pl);
            used += pl;
            out[used] = '\0';
        }
        const enum geist_status status =
            geist_session_prefill_tokens(adapter->session, 1u, &best);
        if (status != GEIST_OK) {
            return map_geist_status(status);
        }
        result->tokens_decoded += 1u;
    }
    /* Force-complete (#34 robustness): greedy token-by-token can dead-end on a
     * partial (e.g. "buil" of "build.run") when the tokenizer offers no clean
     * continuation. If what we have is a prefix of exactly one candidate, append
     * the rest so the emitted value is a valid, complete name; the following
     * scaffold literal re-anchors the KV, so output correctness is enough. */
    if (!spg_choice_complete(names, names_n, out)) {
        const char *trimmed = out;
        while (*trimmed == ' ' || *trimmed == '\t') {
            trimmed += 1;
        }
        const size_t tl    = strlen(trimmed);
        int          match = -1;
        for (size_t i = 0u; i < names_n; i += 1u) {
            if (names[i] != nullptr && strncmp(names[i], trimmed, tl) == 0) {
                match = match < 0 ? (int) i : -2; /* -2 = ambiguous, don't force */
            }
        }
        if (match >= 0) {
            const char  *suffix = names[(size_t) match] + tl;
            const size_t sl     = strlen(suffix);
            if (sl > 0u) {
                const enum spg_status as = append_bytes(result, sl, suffix);
                if (as != SPG_OK) {
                    return as;
                }
            }
        }
    }
    return SPG_OK;
}

/* Free-decode the capability slot masked to the policy capabilities enabled for
 * `kind` (#34). With no matching capability it falls back to a free string —
 * the form stays valid, the policy gate then decides. */
static enum spg_status decode_capability_slot(
    struct spg_model_adapter *adapter, struct spg_model_generate_result *result,
    const enum spg_action_kind kind) {
    const char *names[SPG_POLICY_MAX_CAPABILITIES];
    size_t      names_n = 0u;
    for (size_t i = 0u;
         i < adapter->capability_count && names_n < SPG_POLICY_MAX_CAPABILITIES;
         i += 1u) {
        if (adapter->capabilities[i].kind == kind &&
            adapter->capabilities[i].name != nullptr) {
            names[names_n] = adapter->capabilities[i].name;
            names_n += 1u;
        }
    }
    if (names_n == 0u) {
        return decode_string_slot(adapter, result); /* no mask: free-decode */
    }
    char chosen[128];
    return decode_choice_slot(adapter, result, names, names_n, chosen,
                              sizeof chosen);
}

/* The local_shell `command` slot (#56): mask the FIRST WORD to the command
 * menu, then free-decode the arguments. Only the program name is enumerable —
 * arguments are where a command carries its meaning — so this is a two-phase
 * slot rather than one choice.
 *
 * With no menu it degrades to a plain free-decoded string, which is exactly
 * what `command_mask false` wants and needs no second scaffold. */
static enum spg_status decode_command_slot(
    struct spg_model_adapter *adapter, struct spg_model_generate_result *result) {
    if (adapter->command_names == nullptr || adapter->command_name_count == 0u) {
        return decode_string_slot(adapter, result);
    }
    char chosen[64];
    const enum spg_status cs =
        decode_choice_slot(adapter, result, adapter->command_names,
                           adapter->command_name_count, chosen, sizeof chosen);
    if (cs != SPG_OK) {
        return cs;
    }
    /* The arguments. The slot stops at the closing quote as usual, so a model
     * that wants a bare command simply emits nothing here. */
    return decode_string_slot(adapter, result);
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

    /* Constrained structural decode (#34): build a valid (recommend ...) form
     * by construction. (1) force the fixed opening the model cannot reliably
     * start; (2) mask the kind slot to a valid enum name; (3) emit the field
     * scaffold for that kind, letting the model free-decode only the leaf
     * string values. The free decode loop below is skipped entirely. */
    const bool constrained =
        adapter->force_prefix != nullptr && adapter->force_prefix[0] != '\0';
    if (constrained) {
        /* 0. #125: optional reason-first — the model thinks in a comment line
         * before the forced decision, conditioning the KV. Off (budget 0) is
         * byte-identical to the original constrained path. */
        const enum spg_status reason_as =
            emit_reason_prefix(adapter, result, adapter->reason_budget);
        if (reason_as != SPG_OK) {
            return reason_as;
        }
        /* 1. forced opening "(recommend (kind " */
        const enum spg_status prefix_as =
            emit_literal(adapter, result, adapter->force_prefix);
        if (prefix_as != SPG_OK) {
            return prefix_as;
        }

        /* 2. kind-slot mask: constrain the slot to a valid kind enum name. */
        char        kindbuf[64];
        /* Room for every kind in ALL_KINDS. This was 8 when there were 7
         * kinds; growing the enum would have silently truncated the menu,
         * and the dropped names would have been device_write and finish —
         * the actuator and the exit. */
        const char *kn[16];
        const size_t  knn = spg_kind_names(kn, sizeof kn / sizeof kn[0]);
        const enum spg_status ks =
            decode_choice_slot(adapter, result, kn, knn, kindbuf, sizeof kindbuf);
        if (ks != SPG_OK) {
            return ks;
        }

        /* 3. field scaffold: emit the deterministic structure for the chosen
         * kind, decoding only the leaf slots — the capability masked to the
         * policy's enabled caps, the rest free. If the kind never resolved the
         * partial output is left for the repair loop. */
        enum spg_action_kind kind;
        if (spg_kind_from_text(kindbuf, &kind)) {
            const struct spg_scaffold_seg *segs = nullptr;
            const size_t nseg = spg_scaffold_for_kind(kind, &segs);
            for (size_t s = 0u; s < nseg; s += 1u) {
                enum spg_status ss;
                switch (segs[s].kind) {
                case SPG_SCAFFOLD_LITERAL:
                    ss = emit_literal(adapter, result, segs[s].literal);
                    break;
                case SPG_SCAFFOLD_CAPABILITY:
                    ss = decode_capability_slot(adapter, result, kind);
                    break;
                case SPG_SCAFFOLD_NUMBER:
                    /* The missing break here survived exactly as long as no
                     * scaffold could reach a NUM slot: after the number, the
                     * command-slot decoder ran too and wrote garbage where
                     * SEG_DEVICE's closing literal belonged. -Wimplicit-
                     * fallthrough now guards the whole class. */
                    ss = decode_number_slot(adapter, result);
                    break;
                case SPG_SCAFFOLD_COMMAND:
                    ss = decode_command_slot(adapter, result);
                    break;
                case SPG_SCAFFOLD_STRING:
                default:
                    ss = decode_string_slot(adapter, result);
                    break;
                }
                if (ss != SPG_OK) {
                    return ss;
                }
            }
        }
        return SPG_OK;
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
        .kind             = config->kind,
        .force_prefix     = config->force_prefix, /* #34: GEIST-only, borrowed */
        .capabilities     = config->capabilities,
        .capability_count = config->capability_count,
        .command_names      = config->command_names,
        .command_name_count = config->command_name_count,
        .reason_budget      = config->reason_budget,
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

    /* Best-of-N: temperature drives whether the masked choice slots sample; a
     * nonzero RNG seed derived from the sampling seed makes different seeds
     * explore different valid decisions. */
    adapter->temperature = config->sampling.temperature;
    adapter->choice_rng  = config->sampling.random_seed * 2654435761u + 1u;

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
