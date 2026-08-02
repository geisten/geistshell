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

/* The acceptance gate: keep a tentatively-persisted lesson only if the suite's
 * pass count did not drop. Equality keeps (a lesson that neither helps nor hurts
 * is retained, since it may help cases outside the suite). */
[[nodiscard]] static inline bool spg_improve_accept(size_t baseline_passed,
                                                    size_t candidate_passed) {
    return candidate_passed >= baseline_passed;
}

/* Commit the gate's decision for a lesson that was already tentatively saved
 * into the store before re-evaluation: keep it when accepted, otherwise delete
 * it (revert). Sets *kept. Returns SPG_E_INVALID_ARG on null args, SPG_E_IO if
 * the revert delete fails, otherwise SPG_OK. */
[[nodiscard]] enum spg_status
spg_improve_commit(struct spg_mem_store *store,
                   const struct spg_lesson *lesson, bool accepted, bool *kept);

#ifdef __cplusplus
}
#endif

#endif
