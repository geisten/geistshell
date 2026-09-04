/* Phase 3b (#79): the bounded history window. Ring semantics with the
 * classic off-by-one probes at exactly CAP and CAP+1 entries, byte-identical
 * determinism, the window-0 ablation, and unknown values that stay unknown —
 * all pure, no host sampling. */

#include "geistshell/context.h"
#include "geistshell/machine_state.h"

#include <stdio.h>
#include <string.h>

/* A snapshot whose scalars encode the tick, so a rendered entry can be traced
 * back to the push that produced it. */
static struct spg_machine_state state_for(const uint64_t v) {
    struct spg_machine_state s = {
        .cpu_utilisation_bp = 100u * v,
        .load_1_cbp         = v,
        .temperature_mc     = (int64_t)(40000u + v),
        .cpu_freq_khz       = 1500000u,
        .throttle           = SPG_THROTTLE_NONE,
        .process_count      = 170u,
    };
    s.memory.used_bytes      = 1000u + v;
    s.memory.swap_used_bytes = 0u;
    return s;
}

static int render(const struct spg_machine_history *h, char out[],
                  const size_t cap) {
    size_t required = 0u;
    return spg_machine_history_render(h, cap, out, &required) == SPG_OK ? 0
                                                                        : 1;
}

static int test_ring_semantics(void) {
    struct spg_machine_history h;
    spg_machine_history_init(&h, 3u);
    char block[SPG_MACHINE_HISTORY_RENDER_CAP];

    /* Empty but enabled: the explicit empty form, no special case. */
    if (render(&h, block, sizeof block) != 0 ||
        strcmp(block, "(machine-history)") != 0) {
        fprintf(stderr, "  empty: %s\n", block);
        return 1;
    }

    /* Fewer entries than the window: all present, oldest first. */
    struct spg_machine_state s1 = state_for(1u);
    struct spg_machine_state s2 = state_for(2u);
    spg_machine_history_push(&h, 1u, &s1);
    spg_machine_history_push(&h, 2u, &s2);
    if (render(&h, block, sizeof block) != 0) {
        return 1;
    }
    const char *t1 = strstr(block, "(t 1 ");
    const char *t2 = strstr(block, "(t 2 ");
    if (t1 == nullptr || t2 == nullptr || t1 > t2) {
        fprintf(stderr, "  order: %s\n", block);
        return 1;
    }

    /* Exactly at capacity (the window, 3): everything still present. */
    struct spg_machine_state s3 = state_for(3u);
    spg_machine_history_push(&h, 3u, &s3);
    if (render(&h, block, sizeof block) != 0 ||
        strstr(block, "(t 1 ") == nullptr ||
        strstr(block, "(t 3 ") == nullptr) {
        return 1;
    }

    /* One past capacity: the OLDEST falls out, order stays oldest->newest. */
    struct spg_machine_state s4 = state_for(4u);
    spg_machine_history_push(&h, 4u, &s4);
    if (render(&h, block, sizeof block) != 0) {
        return 1;
    }
    if (strstr(block, "(t 1 ") != nullptr) {
        fprintf(stderr, "  t1 survived overflow: %s\n", block);
        return 1;
    }
    const char *o2 = strstr(block, "(t 2 ");
    const char *o4 = strstr(block, "(t 4 ");
    if (o2 == nullptr || o4 == nullptr || o2 > o4) {
        return 1;
    }
    /* the surviving oldest entry carries ITS tick's values, not a shifted
     * neighbour's */
    if (strstr(block, "(t 2 (cpu-load-bp 200)") == nullptr) {
        fprintf(stderr, "  misaligned: %s\n", block);
        return 1;
    }
    return 0;
}

static int test_determinism(void) {
    struct spg_machine_history a, b;
    spg_machine_history_init(&a, 4u);
    spg_machine_history_init(&b, 4u);
    for (uint64_t t = 1u; t <= 6u; t += 1u) {
        struct spg_machine_state s = state_for(t);
        spg_machine_history_push(&a, t, &s);
        spg_machine_history_push(&b, t, &s);
    }
    char one[SPG_MACHINE_HISTORY_RENDER_CAP];
    char two[SPG_MACHINE_HISTORY_RENDER_CAP];
    if (render(&a, one, sizeof one) != 0 || render(&b, two, sizeof two) != 0) {
        return 1;
    }
    return strcmp(one, two) == 0 ? 0 : 1;
}

static int test_window_zero_is_absent(void) {
    struct spg_machine_history h;
    spg_machine_history_init(&h, 0u);
    struct spg_machine_state s = state_for(1u);
    spg_machine_history_push(&h, 1u, &s); /* must be a no-op */
    char   block[64] = "sentinel";
    size_t required  = 77u;
    if (spg_machine_history_render(&h, sizeof block, block, &required) !=
            SPG_OK ||
        required != 0u || block[0] != '\0') {
        return 1; /* disabled renders NOTHING, not an empty form */
    }
    /* a null history is the same statement */
    if (spg_machine_history_render(nullptr, sizeof block, block, &required) !=
            SPG_OK ||
        required != 0u) {
        return 1;
    }
    return 0;
}

static int test_unknown_stays_unknown(void) {
    struct spg_machine_history h;
    spg_machine_history_init(&h, 2u);
    struct spg_machine_state s = state_for(1u);
    s.cpu_utilisation_bp       = SPG_MACHINE_UNKNOWN;
    s.temperature_mc           = SPG_MACHINE_UNKNOWN_S;
    spg_machine_history_push(&h, 1u, &s);
    char block[SPG_MACHINE_HISTORY_RENDER_CAP];
    if (render(&h, block, sizeof block) != 0) {
        return 1;
    }
    if (strstr(block, "(cpu-load-bp unknown)") == nullptr ||
        strstr(block, "(temperature-mc unknown)") == nullptr) {
        fprintf(stderr, "  interpolated: %s\n", block);
        return 1;
    }
    return 0;
}

static int test_limits(void) {
    /* the documented bound really bounds the worst case */
    struct spg_machine_history h;
    spg_machine_history_init(&h, SPG_MACHINE_HISTORY_CAP);
    for (uint64_t t = 1u; t <= SPG_MACHINE_HISTORY_CAP; t += 1u) {
        struct spg_machine_state s = state_for(t);
        s.cpu_utilisation_bp       = SPG_MACHINE_UNKNOWN; /* longest form */
        spg_machine_history_push(&h, UINT64_MAX - 1u, &s);
    }
    char   block[SPG_MACHINE_HISTORY_RENDER_CAP];
    size_t required = 0u;
    if (spg_machine_history_render(&h, sizeof block, block, &required) !=
        SPG_OK) {
        return 1;
    }
    /* a too-small buffer is refused with no partial record */
    char tiny[8] = "sentine";
    if (spg_machine_history_render(&h, sizeof tiny, tiny, &required) !=
            SPG_E_LIMIT ||
        tiny[0] != '\0') {
        return 1;
    }
    /* an oversized window is clamped at init, never trusted at push */
    struct spg_machine_history big;
    spg_machine_history_init(&big, 1000u);
    if (big.window != SPG_MACHINE_HISTORY_CAP) {
        return 1;
    }
    return 0;
}

/* The ablation contract (#71): a null history and a window-0 history render
 * the SAME context bytes as each other — the block is absent, not empty — and
 * an enabled history adds exactly its own block before the machine state. */
static int test_context_ablation_diff(void) {
    static char              one[16384];
    static char              two[16384];
    struct spg_machine_state machine = state_for(7u);
    struct spg_context_view  view    = {};
    size_t                   n1 = 0u, n2 = 0u;

    struct spg_context_sources sources = {.machine = &machine};
    if (spg_context_render(&sources, &view, sizeof one, one, &n1) != SPG_OK) {
        return 1;
    }
    struct spg_machine_history off;
    spg_machine_history_init(&off, 0u);
    struct spg_machine_state pushed = state_for(9u);
    spg_machine_history_push(&off, 1u, &pushed); /* no-op when disabled */
    sources.machine_history = &off;
    if (spg_context_render(&sources, &view, sizeof two, two, &n2) != SPG_OK) {
        return 1;
    }
    if (n1 != n2 || memcmp(one, two, n1) != 0) {
        return 1; /* window 0 must be byte-identical to no history at all */
    }

    struct spg_machine_history on;
    spg_machine_history_init(&on, 2u);
    sources.machine_history = &on;
    if (spg_context_render(&sources, &view, sizeof two, two, &n2) != SPG_OK) {
        return 1;
    }
    if (strstr(two, "(machine-history)") == nullptr) {
        return 1; /* enabled-but-empty is the explicit empty form */
    }
    spg_machine_history_push(&on, 1u, &pushed);
    if (spg_context_render(&sources, &view, sizeof two, two, &n2) != SPG_OK ||
        strstr(two, "(machine-history (t 1 ") == nullptr) {
        return 1;
    }
    /* the window precedes the current snapshot: trend, then now */
    if (strstr(two, "(machine-history") > strstr(two, "(machine-state")) {
        return 1;
    }
    return 0;
}

int main(void) {
    if (test_ring_semantics() != 0) {
        fprintf(stderr, "test_ring_semantics failed\n");
        return 1;
    }
    if (test_determinism() != 0) {
        fprintf(stderr, "test_determinism failed\n");
        return 1;
    }
    if (test_window_zero_is_absent() != 0) {
        fprintf(stderr, "test_window_zero_is_absent failed\n");
        return 1;
    }
    if (test_unknown_stays_unknown() != 0) {
        fprintf(stderr, "test_unknown_stays_unknown failed\n");
        return 1;
    }
    if (test_limits() != 0) {
        fprintf(stderr, "test_limits failed\n");
        return 1;
    }
    if (test_context_ablation_diff() != 0) {
        fprintf(stderr, "test_context_ablation_diff failed\n");
        return 1;
    }
    printf("test_machine_history: PASS\n");
    return 0;
}
