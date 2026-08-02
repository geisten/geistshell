#ifndef GEISTSHELL_GUARD_RING_H
#define GEISTSHELL_GUARD_RING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Positive-guard ring (docs/LEARNING.md P4): a bounded set of runs that
 * finished AND passed their criterion, one per distinct task shape, so a
 * candidate lesson can be gated against "did it break a task that used to
 * work?" — even for diverse ad-hoc tasks the hand-written suite cannot cover.
 *
 * Deduped by shape: recording an existing shape refreshes its guard (case +
 * recency) instead of adding a copy. When full, the least-recently-seen shape
 * is evicted, so coverage is capped by the number of distinct shapes, not by
 * the number of runs. Fixed storage, no allocation. */

#define SPG_GUARD_SHAPE_MAX 128u
#define SPG_GUARD_PATH_MAX  256u
#define SPG_GUARD_EXPECT_MAX 256u
#define SPG_GUARD_RING_CAP  32u

/* A frozen positive case: the shape it represents, the journal to reconstruct
 * its script from (P3), and the criterion substring to judge it (P1). */
struct spg_guard {
    char     shape[SPG_GUARD_SHAPE_MAX + 1u];
    char     journal_path[SPG_GUARD_PATH_MAX + 1u];
    char     expect[SPG_GUARD_EXPECT_MAX + 1u];
    uint64_t last_seen; /* ring clock tick; higher = more recent */
    bool     used;
};

struct spg_guard_ring {
    struct spg_guard slots[SPG_GUARD_RING_CAP];
    uint64_t         clock; /* monotonically increasing on each record */
};

void spg_guard_ring_init(struct spg_guard_ring *ring);

/* Record a positive guard for `shape`. If the shape is present, refresh its
 * case and recency; else insert it, evicting the least-recently-seen shape
 * when the ring is full. shape/journal_path/expect are copied (truncated to
 * their maxima). Returns the slot index used, or SPG_GUARD_RING_CAP on a null
 * argument. */
size_t spg_guard_ring_record(struct spg_guard_ring *ring, const char *shape,
                             const char *journal_path, const char *expect);

/* Number of occupied slots. */
size_t spg_guard_ring_count(const struct spg_guard_ring *ring);

/* Find the guard for `shape`, or nullptr. */
const struct spg_guard *spg_guard_ring_find(const struct spg_guard_ring *ring,
                                            const char *shape);

#ifdef __cplusplus
}
#endif

#endif
