#ifndef GEISTSHELL_GUARD_RING_H
#define GEISTSHELL_GUARD_RING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Positive-guard ring (docs/LEARNING.md P4/P5): a bounded set of runs that
 * finished AND passed their criterion, one per distinct task shape, so a
 * candidate lesson can be gated against "did it break a task that used to
 * work?" — even for diverse ad-hoc tasks the hand-written suite cannot cover.
 *
 * Weg 2 (P5): a guard is re-run LIVE at mint time from its run config (which
 * carries the model, scenario, policy and the (expect) criterion) so the real
 * model reacts to the candidate lesson injected into context — a frozen replay
 * cannot, it ignores context. A guard therefore stores the config path, not a
 * frozen journal.
 *
 * Deduped by shape: recording an existing shape refreshes its guard (config +
 * recency) instead of adding a copy. When full, the least-recently-seen shape
 * is evicted, so coverage is capped by the number of distinct shapes, not by
 * the number of runs. Fixed storage, no allocation. */

#define SPG_GUARD_SHAPE_MAX 128u
#define SPG_GUARD_PATH_MAX  256u
#define SPG_GUARD_RING_CAP  32u

/* A positive case: the shape it represents and the run config that re-runs and
 * judges it live (P1's (expect) field lives in the config). */
struct spg_guard {
    char     shape[SPG_GUARD_SHAPE_MAX + 1u];
    char     config_path[SPG_GUARD_PATH_MAX + 1u];
    uint64_t last_seen; /* ring clock tick; higher = more recent */
    bool     used;
};

struct spg_guard_ring {
    struct spg_guard slots[SPG_GUARD_RING_CAP];
    uint64_t         clock; /* monotonically increasing on each record */
};

void spg_guard_ring_init(struct spg_guard_ring *ring);

/* Record a positive guard for `shape`. If the shape is present, refresh its
 * config and recency; else insert it, evicting the least-recently-seen shape
 * when the ring is full. shape/config_path are copied (truncated to their
 * maxima). Returns the slot index used, or SPG_GUARD_RING_CAP on a null
 * argument. */
size_t spg_guard_ring_record(struct spg_guard_ring *ring, const char *shape,
                             const char *config_path);

/* Number of occupied slots. */
size_t spg_guard_ring_count(const struct spg_guard_ring *ring);

/* Find the guard for `shape`, or nullptr. */
const struct spg_guard *spg_guard_ring_find(const struct spg_guard_ring *ring,
                                            const char *shape);

/* The per-guard veto rule (docs/LEARNING.md P5, Weg 2): a candidate lesson is
 * only rejected by a guard that PASSED without it and now FAILS with it. A
 * guard that already failed at baseline (non-determinism, or an unrelated
 * breakage) carries no signal and never vetoes. True = this guard is fine with
 * the lesson. */
static inline bool spg_guard_survives(bool baseline_passed,
                                      bool trial_passed) {
    return trial_passed || !baseline_passed;
}

/* Live-run one guard config and return whether it passed its (expect). When
 * with_lesson, the candidate lesson is present in the mind-palace so the real
 * model reacts to it; otherwise it is absent (the baseline). The runner owns
 * toggling the store and doing the agent run; the gate only sequences it.
 * ctx is the caller's opaque state. */
typedef bool (*spg_guard_run_fn)(void *ctx, const char *config_path,
                                 bool with_lesson);

/* Gate a candidate lesson against every guard (docs/LEARNING.md P5, Weg 2).
 * For each guard, run it without then with the lesson; the lesson is vetoed
 * the moment a guard that passed at baseline fails with it. Returns true if
 * the lesson survives every guard (no regression). An empty ring accepts. */
bool spg_guard_ring_gate(const struct spg_guard_ring *ring,
                         spg_guard_run_fn run, void *ctx);

#ifdef __cplusplus
}
#endif

#endif
