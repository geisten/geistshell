#ifndef GEISTSHELL_GRAMMAR_MASK_H
#define GEISTSHELL_GRAMMAR_MASK_H

#include <stdbool.h>

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

#ifdef __cplusplus
}
#endif

#endif
