#ifndef GEISTSHELL_GRAMMAR_MASK_H
#define GEISTSHELL_GRAMMAR_MASK_H

#include "geistshell/model_adapter.h" /* struct spg_model_capability */
#include "geistshell/policy.h"        /* enum spg_action_kind */
#include "geistshell/policy_config.h" /* struct spg_policy_config */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Constrained decoding of the (recommend (kind X)) slot (geistshell#34).
 *
 * The valid kind names are the canonical strings of spg_action_kind (via
 * spg_action_kind_to_string) — the parser's own source of truth, so the mask
 * cannot drift from what parse_action_kind accepts. These predicates are pure
 * (no engine, no session) so the mask logic is unit-tested without a GGUF; the
 * model adapter drives the token loop around them. */

/* Would `emitted` immediately followed by `piece` still be a prefix of at
 * least one valid kind name, without overshooting it? An empty piece asks
 * whether `emitted` itself is a live prefix. This is the per-token gate: at the
 * kind slot, only tokens for which this holds may be sampled, so the decoded
 * kind is valid by construction. piece may be null (treated as ""). */
[[nodiscard]] bool spg_kind_prefix_ok(const char *emitted, const char *piece);

/* Is `emitted` exactly one valid kind name? True terminates the kind slot (no
 * valid kind name is a prefix of another, so completion is unambiguous). */
[[nodiscard]] bool spg_kind_complete(const char *emitted);

/* Resolve a decoded kind name (leading detok whitespace tolerated) to its enum.
 * Returns false if it is not a valid kind. */
[[nodiscard]] bool spg_kind_from_text(const char *emitted,
                                      enum spg_action_kind *out);

/* Fill `out` (capacity `cap`, need >= 7) with the valid kind names and return
 * the count, so the caller can drive the kind slot through the general choice
 * mask below. */
size_t spg_kind_names(const char **out, size_t cap);

/* General choice mask (kind is the special case above). Would `emitted` + `piece`
 * still be a live prefix of at least one of the `n` candidate names, without
 * overshooting? Leading whitespace in the comparison is ignored, so a
 * detokenized " sim.act" matches "sim.act". Used to constrain a value slot
 * (e.g. capability) to a fixed vocabulary. */
[[nodiscard]] bool spg_choice_prefix_ok(const char *const *names, size_t n,
                                        const char *emitted, const char *piece);

/* Is `emitted` exactly one of the `n` candidate names? Assumes no candidate is a
 * prefix of another (true for policy capability names). */
[[nodiscard]] bool spg_choice_complete(const char *const *names, size_t n,
                                       const char *emitted);

/* Field scaffold (geistshell#34 stage 2). Once the kind is fixed, the rest of a
 * valid (recommend ...) form is deterministic structure with a few free leaf
 * values. A scaffold is that structure as an ordered segment list: a LITERAL
 * segment is emitted verbatim (via prefill_tokens); a model-string segment
 * (literal == nullptr) is one free-decoded string value, stopped at the closing
 * quote. Emitting the literals with the model filling the string slots yields a
 * schema-valid form by construction. The bureaucratic fields (cost,
 * uses_network, confidence_bp) are baked into the literals with sane defaults —
 * they are deterministic per kind, not a decision the small model must make. */
enum spg_scaffold_seg_kind {
    SPG_SCAFFOLD_LITERAL = 0, /* emit `literal` verbatim */
    SPG_SCAFFOLD_STRING,      /* one free-decoded string value */
    SPG_SCAFFOLD_CAPABILITY,  /* one string value masked to the policy caps */
    /* one bare integer, digits and an optional leading minus. Needed because
     * every number in the scaffold used to be a literal — cost and confidence
     * are fixed, so the decoder never had to produce one. A device setpoint is
     * the first number the model actually chooses. */
    SPG_SCAFFOLD_NUMBER,
};

struct spg_scaffold_seg {
    enum spg_scaffold_seg_kind kind;
    const char *literal; /* set for SPG_SCAFFOLD_LITERAL, else nullptr */
};

/* The scaffold for `kind`, to emit right after the kind name (the caller has
 * already emitted "(recommend (kind <name>"). Writes the segment array to
 * *out and returns its length (0 for an unknown kind). */
[[nodiscard]] size_t spg_scaffold_for_kind(enum spg_action_kind kind,
                                           const struct spg_scaffold_seg **out);

/* The capability mask table for a policy, ready for
 * spg_model_adapter_config.capabilities. Only ENABLED capabilities are
 * included — a disabled one would let the decoder emit a value the policy gate
 * then denies, which wastes a step and teaches the model nothing. A memory
 * capability expands to one entry per memory_* action kind, because the mask is
 * applied per chosen kind.
 *
 * Names live in the policy TEXT as spans, but the adapter borrows
 * null-terminated strings that must outlive it — so they are copied into
 * name_buf (caller-provided, no allocation) and out[].name points into it.
 * name_buf must outlive the adapter just as out[] does.
 *
 * Returns the number of entries written. Entries stop at out_cap or when
 * name_buf is full; a truncated table is still valid (a narrower mask), never
 * malformed. Every caller that wants `agent --constrained` behaviour must build
 * its table through here — `eval` building its own was how the two ended up
 * running different decoders. */
size_t spg_model_capabilities_from_policy(
    const struct spg_policy_config *policy, size_t text_n, const char text[],
    size_t name_buf_cap, char name_buf[], size_t out_cap,
    struct spg_model_capability out[]);

/* Capacity out[] needs so no enabled capability is dropped: the memory kind
 * expands three-fold. */
#define SPG_MODEL_CAPABILITY_MAX (SPG_POLICY_MAX_CAPABILITIES * 3u)

#ifdef __cplusplus
}
#endif

#endif
