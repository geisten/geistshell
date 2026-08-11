#ifndef GEISTSHELL_MODEL_PROFILE_H
#define GEISTSHELL_MODEL_PROFILE_H

#include "geistshell/sexpr.h"
#include "geistshell/status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* How to drive one particular model (#54).
 *
 *   (model_profile
 *     (name "bitnet-full")
 *     (arch "bitnet-b1.58")
 *     (template llama3))       ; auto | none | gemma | llama3 | generic
 *
 * Deliberately only what is applied. An earlier version also parsed
 * constrained, temperature and best_of, validated them, stored them — and
 * never read them anywhere. A config field nobody consumes is worse than a
 * missing one: it reads like a promise.
 *
 * geistshell drives one governed loop for models that are not comparable, and
 * until now treated them identically: one raw s-expression prompt for all of
 * them. Phase 12 (#72) measured what that costs — with the same constrained
 * decoder, Gemma parsed 9 of 9 machine-diagnosis answers and BitNet b1.58
 * parsed 1 of 9, returning a lone backslash in one case. That is not a model
 * choosing badly; it is a model that was never told what shape to answer in.
 *
 * Data rather than flags, for one decisive reason: a profile file is
 * versionable and journalable. When a benchmark prints a number, the journal
 * has to record which profile produced it, or in three months nobody can
 * reproduce the run. CLI flags cannot do that. */

enum spg_chat_template {
    /* Decide from the model's architecture; falls back to NONE. */
    SPG_TEMPLATE_AUTO = 0,
    /* Raw prompt, no turn markers — what every model got before this existed.
     * Correct for a base model that was never trained on a chat format. */
    SPG_TEMPLATE_NONE,
    SPG_TEMPLATE_GEMMA,
    SPG_TEMPLATE_LLAMA3,
    /* Plain role labels. For a model with no known format that still does
     * better with a visible boundary between instruction and answer than with
     * none at all. */
    SPG_TEMPLATE_GENERIC,
};

#define SPG_PROFILE_NAME_CAP 64u

struct spg_model_profile {
    char name[SPG_PROFILE_NAME_CAP];
    char arch[SPG_PROFILE_NAME_CAP];
    /* Named chat_template, not template: the header is extern "C" guarded for
     * C++ consumers, where `template` is a keyword. */
    enum spg_chat_template chat_template;
    bool                   present;
};

[[nodiscard]] enum spg_status spg_model_profile_load(
    size_t input_n, const char input[], size_t token_capacity,
    struct spg_sexpr_token tokens[static token_capacity], size_t node_capacity,
    struct spg_sexpr_node     nodes[static node_capacity],
    struct spg_model_profile *out);

/* Pick a template from an architecture string when the profile says `auto`.
 * Unknown architectures get NONE: guessing a format a model was not trained on
 * is worse than sending none, because the markers then appear as ordinary
 * tokens in the prompt. */
[[nodiscard]] enum spg_chat_template spg_template_for_arch(const char *arch);

/* Wrap a prompt in the template's turn markers.
 *
 * system may be null (no system turn) — templates that have no system role
 * ignore it either way. The result always ends with the marker that opens the
 * model's turn, so the decoder continues from there rather than from the end
 * of the user's text.
 *
 * SPG_E_LIMIT when the framed prompt would not fit; dst is then empty, because
 * half a chat template is worse than none. */
[[nodiscard]] enum spg_status
spg_chat_frame(enum spg_chat_template tmpl, const char *system, size_t user_n,
               const char user[], size_t dst_capacity,
               char dst[static dst_capacity], size_t *out_used);

[[nodiscard]] const char *
spg_chat_template_to_string(enum spg_chat_template tmpl);

#ifdef __cplusplus
}
#endif

#endif
