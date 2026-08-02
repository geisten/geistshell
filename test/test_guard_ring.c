/* P4 (docs/LEARNING.md): the capability-set shape key and the positive-guard
 * ring. The shape is the deduplicated, sorted set of "<kind>:<capability>"
 * tokens; the ring dedups by shape and evicts the least-recently-seen shape
 * when full. */
#include "geistshell/eval.h"
#include "geistshell/guard_ring.h"

#include <stdio.h>
#include <string.h>

static int test_shape_key(void) {
    /* two shell steps with distinct capabilities + a finish: the shape is the
     * sorted SET, so order and repetition do not change it */
    const char r0[] =
        "(recommend (kind local_shell) (capability \"proc.exec\") (cost 1) "
        "(uses_network false) (confidence_bp 6000) (reason \"a\") "
        "(command \"echo a\"))";
    const char r1[] =
        "(recommend (kind local_shell) (capability \"fs.write\") (cost 1) "
        "(uses_network false) (confidence_bp 6000) (reason \"b\") "
        "(command \"tee f\"))";
    const char r2[] = "(recommend (kind finish) (reason \"done\"))";
    struct spg_fake_response a[] = {
        {sizeof r0 - 1u, r0}, {sizeof r1 - 1u, r1}, {sizeof r2 - 1u, r2}};
    char   shape[256];
    size_t len = 0u;
    if (spg_shape_from_script(a, 3u, sizeof shape, shape, &len) != SPG_OK) {
        return 1;
    }
    /* sorted set: finish, local_shell:fs.write, local_shell:proc.exec */
    if (strcmp(shape, "finish+local_shell:fs.write+local_shell:proc.exec") != 0) {
        fprintf(stderr, "shape=%s\n", shape);
        return 1;
    }
    /* same capabilities, different order + a duplicate -> identical shape */
    struct spg_fake_response b[] = {
        {sizeof r1 - 1u, r1}, {sizeof r0 - 1u, r0},
        {sizeof r0 - 1u, r0}, {sizeof r2 - 1u, r2}};
    char   shape2[256];
    size_t len2 = 0u;
    if (spg_shape_from_script(b, 4u, sizeof shape2, shape2, &len2) != SPG_OK ||
        strcmp(shape, shape2) != 0) {
        return 1;
    }
    return 0;
}

static int test_ring_dedup_and_lru(void) {
    struct spg_guard_ring ring;
    spg_guard_ring_init(&ring);

    /* two distinct shapes -> two guards */
    spg_guard_ring_record(&ring, "shape-A", "/cfg/a.spg");
    spg_guard_ring_record(&ring, "shape-B", "/cfg/b.spg");
    if (spg_guard_ring_count(&ring) != 2u) {
        return 1;
    }

    /* re-recording a shape refreshes, does not duplicate */
    spg_guard_ring_record(&ring, "shape-A", "/cfg/a2.spg");
    if (spg_guard_ring_count(&ring) != 2u) {
        return 1;
    }
    const struct spg_guard *a = spg_guard_ring_find(&ring, "shape-A");
    if (a == nullptr || strcmp(a->config_path, "/cfg/a2.spg") != 0) {
        return 1;
    }

    /* fill the ring, then one more distinct shape evicts the least-recently
     * seen. shape-B was recorded before shape-A's refresh, so among the
     * originals it is the oldest; touch shape-B to make shape-C-... the LRU. */
    char name[32];
    for (unsigned i = 0u; i < SPG_GUARD_RING_CAP; i++) {
        snprintf(name, sizeof name, "fill-%u", i);
        spg_guard_ring_record(&ring, name, "/cfg/f.spg");
    }
    /* ring is full at CAP distinct shapes */
    if (spg_guard_ring_count(&ring) != SPG_GUARD_RING_CAP) {
        return 1;
    }
    /* the very first inserted-and-never-refreshed shape must have been evicted
     * by now (many distinct inserts past capacity) */
    if (spg_guard_ring_find(&ring, "shape-B") != nullptr) {
        return 1;
    }
    /* a fresh distinct shape still fits by evicting an LRU, never grows past cap */
    spg_guard_ring_record(&ring, "shape-Z", "/cfg/z.spg");
    if (spg_guard_ring_count(&ring) != SPG_GUARD_RING_CAP ||
        spg_guard_ring_find(&ring, "shape-Z") == nullptr) {
        return 1;
    }
    return 0;
}

/* P5 (Weg 2): only a guard that passed without the lesson and fails with it
 * vetoes; an already-failing guard carries no signal. */
static int test_guard_veto_rule(void) {
    if (!spg_guard_survives(true, true)) {   /* passed both -> fine */
        return 1;
    }
    if (spg_guard_survives(true, false)) {   /* pass -> fail: the veto */
        return 1;
    }
    if (!spg_guard_survives(false, false)) { /* already broken -> no signal */
        return 1;
    }
    if (!spg_guard_survives(false, true)) {  /* lesson even helped -> fine */
        return 1;
    }
    return 0;
}

/* A fake live-runner: a designated "regressing" guard passes at baseline and
 * fails with the lesson; all others pass both ways. Records the call sequence. */
struct fake_runner {
    const char *regressing_config; /* the guard that breaks under the lesson */
    int         calls;
};
static bool fake_run(void *ctx, const char *config_path, bool with_lesson) {
    struct fake_runner *f = (struct fake_runner *)ctx;
    f->calls++;
    if (f->regressing_config != nullptr &&
        strcmp(config_path, f->regressing_config) == 0) {
        return !with_lesson; /* passes baseline, fails with the lesson */
    }
    return true;
}

/* P5 (Weg 2): the gate accepts when every guard survives, vetoes the moment a
 * guard that passed at baseline fails with the lesson. */
static int test_guard_gate(void) {
    struct spg_guard_ring ring;
    spg_guard_ring_init(&ring);
    spg_guard_ring_record(&ring, "shape-A", "/cfg/a.spg");
    spg_guard_ring_record(&ring, "shape-B", "/cfg/b.spg");

    /* no guard regresses -> the lesson survives */
    struct fake_runner ok = {.regressing_config = nullptr};
    if (!spg_guard_ring_gate(&ring, fake_run, &ok) || ok.calls != 4) {
        return 1; /* 2 guards x (baseline + trial) */
    }

    /* one guard regresses under the lesson -> vetoed */
    struct fake_runner bad = {.regressing_config = "/cfg/b.spg"};
    if (spg_guard_ring_gate(&ring, fake_run, &bad)) {
        return 1;
    }

    /* an empty ring accepts; a null runner is rejected */
    struct spg_guard_ring empty;
    spg_guard_ring_init(&empty);
    struct fake_runner e = {.regressing_config = nullptr};
    if (!spg_guard_ring_gate(&empty, fake_run, &e) ||
        spg_guard_ring_gate(&ring, nullptr, &e)) {
        return 1;
    }
    return 0;
}

int main(void) {
    if (test_shape_key() != 0) {
        fprintf(stderr, "test_shape_key failed\n");
        return 1;
    }
    if (test_ring_dedup_and_lru() != 0) {
        fprintf(stderr, "test_ring_dedup_and_lru failed\n");
        return 1;
    }
    if (test_guard_veto_rule() != 0) {
        fprintf(stderr, "test_guard_veto_rule failed\n");
        return 1;
    }
    if (test_guard_gate() != 0) {
        fprintf(stderr, "test_guard_gate failed\n");
        return 1;
    }
    printf("test_guard_ring: PASS\n");
    return 0;
}
