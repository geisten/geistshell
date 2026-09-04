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
    /* sorted set (finish excluded — universal): local_shell:fs.write, local_shell:proc.exec */
    if (strcmp(shape, "local_shell:fs.write+local_shell:proc.exec") != 0) {
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

/* #12 (decision 6): the sequence mode. Two trajectories that COLLIDE under
 * the set key (same capabilities, different order) get distinct sequence
 * keys — so a guard minted for one no longer silently stands in for the
 * other. The set mode stays the default and is untouched. */
static int test_shape_sequence_mode(void) {
    const char r0[] =
        "(recommend (kind local_shell) (capability \"proc.exec\") (cost 1) "
        "(uses_network false) (confidence_bp 6000) (reason \"a\") "
        "(command \"echo a\"))";
    const char r1[] =
        "(recommend (kind local_shell) (capability \"fs.write\") (cost 1) "
        "(uses_network false) (confidence_bp 6000) (reason \"b\") "
        "(command \"tee f\"))";
    const char r2[] = "(recommend (kind finish) (reason \"done\"))";
    struct spg_fake_response exec_then_write[] = {
        {sizeof r0 - 1u, r0}, {sizeof r1 - 1u, r1}, {sizeof r2 - 1u, r2}};
    struct spg_fake_response write_then_exec[] = {
        {sizeof r1 - 1u, r1}, {sizeof r0 - 1u, r0}, {sizeof r2 - 1u, r2}};

    /* the collision, demonstrated: identical SET keys */
    char   set_a[256], set_b[256];
    size_t len = 0u;
    if (spg_shape_from_script(exec_then_write, 3u, sizeof set_a, set_a,
                              &len) != SPG_OK ||
        spg_shape_from_script(write_then_exec, 3u, sizeof set_b, set_b,
                              &len) != SPG_OK ||
        strcmp(set_a, set_b) != 0) {
        return 1;
    }

    /* the resolution: distinct SEQUENCE keys, ordered, '>'-joined */
    char seq_a[256], seq_b[256];
    if (spg_shape_from_script_mode(exec_then_write, 3u,
                                   SPG_SHAPE_MODE_SEQUENCE, sizeof seq_a,
                                   seq_a, &len) != SPG_OK ||
        spg_shape_from_script_mode(write_then_exec, 3u,
                                   SPG_SHAPE_MODE_SEQUENCE, sizeof seq_b,
                                   seq_b, &len) != SPG_OK) {
        return 1;
    }
    if (strcmp(seq_a, "local_shell:proc.exec>local_shell:fs.write") != 0 ||
        strcmp(seq_b, "local_shell:fs.write>local_shell:proc.exec") != 0) {
        fprintf(stderr, "seq_a=%s seq_b=%s\n", seq_a, seq_b);
        return 1;
    }

    /* consecutive duplicates collapse (a retried step is the same step),
     * but a capability revisited LATER stays a distinct trajectory */
    struct spg_fake_response retried[] = {
        {sizeof r0 - 1u, r0}, {sizeof r0 - 1u, r0}, {sizeof r1 - 1u, r1}};
    char seq_r[256];
    if (spg_shape_from_script_mode(retried, 3u, SPG_SHAPE_MODE_SEQUENCE,
                                   sizeof seq_r, seq_r, &len) != SPG_OK ||
        strcmp(seq_r, seq_a) != 0) {
        return 1;
    }
    struct spg_fake_response revisit[] = {
        {sizeof r0 - 1u, r0}, {sizeof r1 - 1u, r1}, {sizeof r0 - 1u, r0}};
    char seq_v[256];
    if (spg_shape_from_script_mode(revisit, 3u, SPG_SHAPE_MODE_SEQUENCE,
                                   sizeof seq_v, seq_v, &len) != SPG_OK ||
        strcmp(seq_v, seq_a) == 0) {
        return 1;
    }

    /* SET mode via the mode entry point is byte-identical to the default */
    char via_mode[256];
    if (spg_shape_from_script_mode(exec_then_write, 3u, SPG_SHAPE_MODE_SET,
                                   sizeof via_mode, via_mode,
                                   &len) != SPG_OK ||
        strcmp(via_mode, set_a) != 0) {
        return 1;
    }

    /* the guard ring dedups by the finer key: both order-variants now hold a
     * guard, still bounded by distinct shapes (LRU-capped as ever) */
    struct spg_guard_ring ring;
    spg_guard_ring_init(&ring);
    spg_guard_ring_record(&ring, seq_a, "/cfg/a.spg");
    spg_guard_ring_record(&ring, seq_b, "/cfg/b.spg");
    spg_guard_ring_record(&ring, seq_a, "/cfg/a.spg"); /* dedup, no growth */
    if (spg_guard_ring_count(&ring) != 2u ||
        spg_guard_ring_find(&ring, seq_a) == nullptr ||
        spg_guard_ring_find(&ring, seq_b) == nullptr) {
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
    if (test_shape_sequence_mode() != 0) {
        fprintf(stderr, "test_shape_sequence_mode failed\n");
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
