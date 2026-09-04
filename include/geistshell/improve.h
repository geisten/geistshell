#ifndef GEISTSHELL_IMPROVE_H
#define GEISTSHELL_IMPROVE_H

#include "geistshell/eval.h"
#include "geistshell/mem_store.h"
#include "geistshell/status.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Self-improvement: turn eval failures into durable lessons in the mind-palace,
 * each accepted only if it does not regress the suite (the eval harness is the
 * acceptance gate for the agent's self-modifications). This header holds the
 * two pure decisions — distilling a lesson and accepting it; the orchestration
 * (run suite -> reflect -> persist -> re-run -> keep/revert) is driven on top. */

#define SPG_LESSON_BODY_MAX 1024u

/* A lesson is a mind-palace memory: a stable slug (so the same failure mode
 * dedups/updates rather than piling up), a one-line description for the index,
 * and the guidance body recalled into future context. */
struct spg_lesson {
    char slug[SPG_MEM_SLUG_MAX + 1u];
    char description[SPG_MEM_DESC_MAX + 1u];
    char body[SPG_LESSON_BODY_MAX + 1u];
};

/* Distill a lesson from a failed case, keyed on the agent's termination (the
 * actionable failure mode). Returns false (out untouched) when the case passed
 * or finished but merely missed an eval expectation — there is no agent-level
 * lesson to learn there. Deterministic. */
[[nodiscard]] bool spg_reflect_case(const struct spg_eval_case_result *result,
                                    struct spg_lesson *out);

/* Distill a lesson from a FINISHED-but-criterion-failed run (docs/LEARNING.md
 * P2) — the class spg_reflect_case cannot judge, because the failure is in the
 * outcome, not the loop's termination. The verifier (P1) supplies the ground
 * truth; this states the concrete miss, it does not invent a fix.
 *
 * slug = "lesson-outcome-<shape_token>", so lessons dedup per task shape.
 * shape_token is the caller's task-shape key (until P4 it may be any stable,
 * non-empty token; P4 supplies the capability-set key). It is sanitized to a
 * mind-palace-safe slug ([a-z0-9-], truncated). The description is a terse
 * directive; the body quotes observed-vs-expected verbatim. Whether stating
 * the miss helps is decided later by slug-recurrence (P7), never assumed here.
 *
 * Returns false (out untouched) on null/empty args. Deterministic. */
[[nodiscard]] bool spg_reflect_outcome(const char *shape_token,
                                       const char *expected_substring,
                                       const char *observation,
                                       struct spg_lesson *out);

/* Distil a reusable SKILL from a PASSING trajectory (docs/LEARNING.md /
 * geistshell#26): the success-side counterpart to reflect. Where a lesson says
 * "do not do the broken thing", a skill says "here is how a task of this shape
 * was done". Keyed by the capability shape (P4) so it dedups per task kind and
 * a later same-shape task recalls it.
 *
 * slug = "skill-<shape_token>". The description is a one-line procedure
 * directive; the body lists the ordered action kinds of the successful run.
 * Deterministic — a template over the trajectory, no model (a model-driven
 * distillation is the offline follow-up in #26). procedure_summary is a short
 * caller-built string of the ordered kinds (e.g. "local_shell -> finish").
 *
 * Returns false (out untouched) on null/empty args. */
[[nodiscard]] bool spg_reflect_skill(const char *shape_token,
                                     const char *procedure_summary,
                                     struct spg_lesson *out);

/* The acceptance gate: keep a tentatively-persisted lesson only if the suite's
 * pass count did not drop. Equality keeps (a lesson that neither helps nor hurts
 * is retained, since it may help cases outside the suite). */
[[nodiscard]] static inline bool spg_improve_accept(size_t baseline_passed,
                                                    size_t candidate_passed) {
    return candidate_passed >= baseline_passed;
}

/* #11 (LEARNING.md decision 3): the full keep/revert composition, including
 * the OPT-IN benefit proof. Default (prove_benefit false) is the historical
 * regression-only gate: suite pass count did not drop AND no live guard
 * vetoed; benefit stays a longitudinal slug-recurrence question (P7). With
 * prove_benefit, the gate is STRICTLY tighter: the candidate's own failing
 * case, re-run live with the lesson present, must additionally now pass —
 * for rare failure types whose recurrence signal would confirm benefit only
 * weeks later. case_now_passes is ignored without the flag, so callers can
 * pass anything (conventionally false). Pure; the live re-run itself is the
 * caller's injected concern, same as the guard-ring runner. */
[[nodiscard]] static inline bool spg_improve_gate(bool suite_ok, bool guards_ok,
                                                  bool prove_benefit,
                                                  bool case_now_passes) {
    return suite_ok && guards_ok && (!prove_benefit || case_now_passes);
}

/* Commit the gate's decision for a lesson that was already tentatively saved
 * into the store before re-evaluation: keep it when accepted, otherwise delete
 * it (revert). Sets *kept. Returns SPG_E_INVALID_ARG on null args, SPG_E_IO if
 * the revert delete fails, otherwise SPG_OK. */
[[nodiscard]] enum spg_status
spg_improve_commit(struct spg_mem_store *store,
                   const struct spg_lesson *lesson, bool accepted, bool *kept);

/* --- GEPA-lite: evolve the injected directive against the gate (#27) ------
 *
 * A lesson's DESCRIPTION is the one line a small model sees per tick. Today it
 * is minted once and never refined. GEPA-lite searches, OFFLINE, for a wording
 * that flips more cases through the SAME acceptance gate (P5) — the runtime is
 * untouched: the model still sees one budgeted directive, only WHICH text is
 * stored changes. The description budget (SPG_MEM_DESC_MAX) is a HARD fitness
 * constraint, so evolution can never bloat the injected line. */

enum spg_gepa_op {
    SPG_GEPA_OP_IDENTITY = 0, /* the seed itself — always in the population */
    SPG_GEPA_OP_TRUNCATE,     /* keep only the first sentence */
    SPG_GEPA_OP_TIGHTEN,      /* drop a trailing parenthetical aside */
    SPG_GEPA_OP_CUE_FIRST,    /* lead with the concrete cue, then the seed */
    SPG_GEPA_OP_COUNT,
};

/* Apply one deterministic mutation operator to `seed`, writing a
 * NUL-terminated variant into out[0..cap). `cue` is the concrete snippet a
 * run supplies (the reject/deny phrase, or an observation fragment) that the
 * cue operator splices in; it may be null/empty, in which case operators that
 * need it produce nothing.
 *
 * Returns the variant's length, or 0 when the operator does not apply to this
 * seed (e.g. TIGHTEN on a seed with no parenthetical), when the result would
 * be empty, when it would not differ from the seed, or when it would not fit
 * in cap (the budget as a hard constraint). A 0 return leaves out[] an empty
 * string. Pure and deterministic: identical (op, seed, cue) always yields
 * identical bytes — the property that makes the evolved directive replayable. */
size_t spg_gepa_mutate(enum spg_gepa_op op, const char *seed, const char *cue,
                       size_t cap, char out[]);

/* Pick the fittest of a candidate population. scores[i] is candidate i's
 * measured pass count through the gate; higher is fitter. The incumbent
 * (index 0 by convention — the seed) wins every tie, so a mutation is adopted
 * only when it STRICTLY beats the seed: evolution never trades a proven
 * wording for an equal-scoring guess. Returns the winning index, or 0 when n
 * is 0. Pure. */
[[nodiscard]] size_t spg_gepa_select(size_t n, const size_t scores[]);

#ifdef __cplusplus
}
#endif

#endif
