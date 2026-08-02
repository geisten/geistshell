#include "geistshell/guard_ring.h"

#include <stdio.h>
#include <string.h>

void spg_guard_ring_init(struct spg_guard_ring *ring) {
    if (ring == nullptr) {
        return;
    }
    *ring = (struct spg_guard_ring){};
}

static void copy_field(char *dst, size_t cap, const char *src) {
    if (src == nullptr) {
        dst[0] = '\0';
        return;
    }
    (void)snprintf(dst, cap, "%s", src);
}

size_t spg_guard_ring_record(struct spg_guard_ring *ring, const char *shape,
                             const char *journal_path, const char *expect) {
    if (ring == nullptr || shape == nullptr) {
        return SPG_GUARD_RING_CAP;
    }
    ring->clock += 1u;

    size_t free_slot = SPG_GUARD_RING_CAP;
    size_t lru_slot  = 0u;
    for (size_t i = 0u; i < SPG_GUARD_RING_CAP; i++) {
        struct spg_guard *g = &ring->slots[i];
        if (g->used && strcmp(g->shape, shape) == 0) {
            /* refresh the existing shape's case + recency */
            copy_field(g->journal_path, sizeof g->journal_path, journal_path);
            copy_field(g->expect, sizeof g->expect, expect);
            g->last_seen = ring->clock;
            return i;
        }
        if (!g->used && free_slot == SPG_GUARD_RING_CAP) {
            free_slot = i;
        }
        if (ring->slots[i].last_seen < ring->slots[lru_slot].last_seen) {
            lru_slot = i;
        }
    }

    /* new shape: fill a free slot, else evict the least-recently-seen one */
    const size_t slot = (free_slot != SPG_GUARD_RING_CAP) ? free_slot : lru_slot;
    struct spg_guard *g = &ring->slots[slot];
    copy_field(g->shape, sizeof g->shape, shape);
    copy_field(g->journal_path, sizeof g->journal_path, journal_path);
    copy_field(g->expect, sizeof g->expect, expect);
    g->last_seen = ring->clock;
    g->used      = true;
    return slot;
}

size_t spg_guard_ring_count(const struct spg_guard_ring *ring) {
    if (ring == nullptr) {
        return 0u;
    }
    size_t n = 0u;
    for (size_t i = 0u; i < SPG_GUARD_RING_CAP; i++) {
        n += ring->slots[i].used ? 1u : 0u;
    }
    return n;
}

const struct spg_guard *spg_guard_ring_find(const struct spg_guard_ring *ring,
                                            const char *shape) {
    if (ring == nullptr || shape == nullptr) {
        return nullptr;
    }
    for (size_t i = 0u; i < SPG_GUARD_RING_CAP; i++) {
        if (ring->slots[i].used && strcmp(ring->slots[i].shape, shape) == 0) {
            return &ring->slots[i];
        }
    }
    return nullptr;
}
