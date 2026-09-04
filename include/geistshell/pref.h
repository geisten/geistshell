#ifndef GEISTSHELL_PREF_H
#define GEISTSHELL_PREF_H

#include "geistshell/mem_store.h"
#include "geistshell/status.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* User-profile memory (geistshell#28): cross-session preferences, a distinct
 * KIND from task lessons/skills. Where a lesson says "the world proved this
 * about the TASK", a preference says "the user showed this about how THEY want
 * choices made". It lives in the same mind-palace store under a `pref-<key>`
 * slug namespace (distinct from `lesson-*`/`skill-*`) and is injected as ONE
 * budgeted context line into later sessions.
 *
 * Two boundaries are load-bearing and enforced here, not merely documented:
 *
 *  1. Write-on-evidence, never model self-assertion. A preference is recorded
 *     only when the WORLD showed it — a user correction, or the same choice
 *     repeated — the same anti-delusion stance as eval-gated learning. A model
 *     "guessing" a preference writes nothing.
 *
 *  2. Capability-invariance. A preference influences FRAMING and DEFAULTS
 *     only; it can never widen what the policy gate permits. This module has
 *     no path to the policy at all — the profile line is context, and the
 *     gate reads the policy, never the profile. */

/* Why a preference is being asserted — the source of the signal. */
enum spg_pref_evidence {
    /* The model inferred it. Never writes: this is the defect being avoided. */
    SPG_PREF_EVIDENCE_ASSERTED = 0,
    /* The user chose the same value again. Writes once the count shows the
     * repetition (>= 2 observations of the same value). */
    SPG_PREF_EVIDENCE_REPEATED,
    /* The user explicitly corrected a choice. Authoritative: writes on the
     * first such signal (count >= 1). */
    SPG_PREF_EVIDENCE_CORRECTION,
};

/* The write-on-evidence decision, pure and total. `observed_count` is how many
 * times the world showed this value (a repeated choice counts up; a correction
 * is one authoritative event). Returns true only when the evidence justifies
 * persisting the preference:
 *   ASSERTED    -> false always (a model self-assertion never writes)
 *   REPEATED    -> observed_count >= 2
 *   CORRECTION  -> observed_count >= 1
 * The threshold lives here, in one testable place, so no caller can lower it. */
[[nodiscard]] bool spg_pref_should_write(enum spg_pref_evidence evidence,
                                         size_t observed_count);

/* Record key=value as a `pref-<key>` memory, but ONLY when the evidence
 * justifies it (spg_pref_should_write). key is sanitised into the slug the
 * same way skill/outcome slugs are; value becomes the one budgeted line (the
 * description) and the body records the provenance (evidence + count) so the
 * profile is auditable. *wrote (may be null) reports whether a write happened.
 *
 * Returns SPG_E_INVALID_ARG on null store/key/value or an empty key/value;
 * SPG_OK with *wrote=false when the evidence was insufficient (not an error —
 * declining to write is the correct behaviour); otherwise the store's status
 * from the underlying save. */
[[nodiscard]] enum spg_status spg_pref_record(struct spg_mem_store *store,
                                              const char *key, const char *value,
                                              enum spg_pref_evidence evidence,
                                              size_t observed_count,
                                              bool  *wrote);

/* Render the whole profile as ONE budgeted line for context injection:
 *
 *   (profile "editor=vim; units=metric")
 *
 * The `pref-*` descriptions are collected slug-sorted and joined with "; ".
 * The line is capped at budget_bytes (0 = SPG_MEM_DESC_MAX): the profile can
 * grow across sessions without ever growing the small model's window beyond
 * this one line — the same context-invariance property as the P6 lesson
 * directive. Writes a NUL-terminated line into dst[0..dst_cap) and returns its
 * length, or 0 when there are no preferences / it does not fit / on bad args
 * (dst left an empty string). Deterministic: identical stores yield identical
 * bytes. */
size_t spg_pref_render(struct spg_mem_store *store, size_t budget_bytes,
                       size_t dst_cap, char dst[]);

#ifdef __cplusplus
}
#endif

#endif
