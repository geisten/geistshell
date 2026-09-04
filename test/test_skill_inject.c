/* #26 B: the shape a run derives BEFORE its first tick, and the journal
 * verdict the mint trigger reads. Pure where possible; the verdict reader is
 * exercised against a journal this test writes itself. */

#include "geistshell/eval.h"
#include "geistshell/improve.h"
#include "geistshell/journal.h"
#include "geistshell/policy_config.h"

#include <stdio.h>
#include <string.h>

static const char policy_text[] =
    "(policy"
    " (network_default deny)"
    " (budgets"
    "  (inference_steps 100)"
    "  (tokens 4096)"
    "  (shell_actions 1)"
    "  (sim_actions 8)"
    "  (wall_ms 60000))"
    " (capability"
    "  ((name sim.act) (kind simulator) (enabled true) (budget 8))"
    "  ((name build.run) (kind local_shell) (enabled true) (budget 1))"
    "  ((name off.cap) (kind local_shell) (enabled false) (budget 1))))";

static int load_policy(struct spg_policy_config *policy) {
    struct spg_sexpr_token         tokens[192];
    struct spg_sexpr_node          nodes[192];
    struct spg_policy_config_error error = {};
    return spg_policy_config_load(strlen(policy_text), policy_text, 192u,
                                  tokens, 192u, nodes, policy,
                                  &error) == SPG_OK
               ? 0
               : 1;
}

/* The policy-derived shape: enabled capabilities only, sorted, the same key
 * language as the trajectory shape — so a skill minted from a passing run is
 * findable by a later run granted the same capabilities. */
static int test_shape_from_policy(void) {
    struct spg_policy_config policy = {};
    if (load_policy(&policy) != 0) {
        return 1;
    }
    char   shape[256];
    size_t len = 0u;
    if (spg_shape_from_policy(strlen(policy_text), policy_text, &policy,
                              sizeof shape, shape, &len) != SPG_OK) {
        return 1;
    }
    /* sorted, '+'-joined, the DISABLED capability absent */
    if (strcmp(shape, "local_shell:build.run+simulator:sim.act") != 0) {
        fprintf(stderr, "  shape=%s\n", shape);
        return 1;
    }

    /* the promise that makes injection work: a trajectory that used exactly
     * the granted capabilities derives the SAME key */
    const char r0[] =
        "(recommend (kind local_shell) (capability \"build.run\") (cost 1) "
        "(uses_network false) (confidence_bp 6000) (reason \"a\") "
        "(command \"echo a\"))";
    const char r1[] =
        "(recommend (kind simulator) (capability \"sim.act\") (cost 1) "
        "(uses_network false) (confidence_bp 6000) (reason \"b\"))";
    const char r2[] = "(recommend (kind finish) (reason \"done\"))";
    struct spg_fake_response script[] = {
        {sizeof r0 - 1u, r0}, {sizeof r1 - 1u, r1}, {sizeof r2 - 1u, r2}};
    char   traj[256];
    size_t tlen = 0u;
    if (spg_shape_from_script(script, 3u, sizeof traj, traj, &tlen) !=
            SPG_OK ||
        strcmp(traj, shape) != 0) {
        fprintf(stderr, "  policy=%s trajectory=%s\n", shape, traj);
        return 1;
    }

    /* ... and both sides land on the same skill slug */
    struct spg_lesson from_policy, from_traj;
    if (!spg_reflect_skill(shape, "-", &from_policy) ||
        !spg_reflect_skill(traj, "local_shell -> finish", &from_traj) ||
        strcmp(from_policy.slug, from_traj.slug) != 0) {
        return 1;
    }

    /* deterministic: twice the same bytes */
    char   again[256];
    size_t alen = 0u;
    if (spg_shape_from_policy(strlen(policy_text), policy_text, &policy,
                              sizeof again, again, &alen) != SPG_OK ||
        strcmp(shape, again) != 0) {
        return 1;
    }

    /* an empty policy yields an empty key, not an error */
    struct spg_policy_config none = {};
    if (spg_shape_from_policy(0u, "", &none, sizeof shape, shape, &len) !=
            SPG_OK ||
        len != 0u) {
        return 1;
    }
    return 0;
}

static int append_text(struct spg_journal_writer *w, const uint64_t ts,
                       const enum spg_journal_event_kind kind,
                       const char *payload) {
    uint64_t seq = 0u;
    return spg_journal_writer_append(w, ts, 0u, kind, SPG_OK, strlen(payload),
                                     (const uint8_t *)payload, &seq) == SPG_OK
               ? 0
               : 1;
}

/* The mint trigger's witness: pass, fail, and — crucially — ABSENT, which is
 * its own state and never silently a pass. */
static int test_journal_verdict(void) {
    const char path[] = "/tmp/geistshell_test_skill_verdict.sgj";

    enum spg_journal_verdict v = SPG_JOURNAL_VERDICT_PASS;
    (void)remove(path);
    if (spg_journal_verdict(path, &v) != SPG_OK ||
        v != SPG_JOURNAL_VERDICT_NONE) {
        return 1; /* a missing journal proves nothing */
    }

    struct spg_journal_writer w = {};
    if (spg_journal_writer_open(&w, path) != SPG_OK) {
        return 1;
    }
    if (append_text(&w, 1u, SPG_JOURNAL_EVENT_MODEL_OUTPUT,
                    "(recommend (kind finish) (reason \"done\"))") != 0) {
        (void)spg_journal_writer_close(&w);
        return 1;
    }
    (void)spg_journal_writer_close(&w);
    if (spg_journal_verdict(path, &v) != SPG_OK ||
        v != SPG_JOURNAL_VERDICT_NONE) {
        return 1; /* a trajectory without a verdict record stays unjudged */
    }

    if (spg_journal_writer_open(&w, path) != SPG_OK ||
        append_text(&w, 2u, SPG_JOURNAL_EVENT_RESULT,
                    "(run_verdict (verdict fail_observation))") != 0) {
        return 1;
    }
    (void)spg_journal_writer_close(&w);
    if (spg_journal_verdict(path, &v) != SPG_OK ||
        v != SPG_JOURNAL_VERDICT_FAIL) {
        return 1;
    }

    if (spg_journal_writer_open(&w, path) != SPG_OK ||
        append_text(&w, 3u, SPG_JOURNAL_EVENT_RESULT,
                    "(run_verdict (verdict pass))") != 0) {
        return 1;
    }
    (void)spg_journal_writer_close(&w);
    if (spg_journal_verdict(path, &v) != SPG_OK ||
        v != SPG_JOURNAL_VERDICT_PASS) {
        return 1; /* the LAST verdict wins */
    }

    (void)remove(path);
    return 0;
}

int main(void) {
    if (test_shape_from_policy() != 0) {
        fprintf(stderr, "test_shape_from_policy failed\n");
        return 1;
    }
    if (test_journal_verdict() != 0) {
        fprintf(stderr, "test_journal_verdict failed\n");
        return 1;
    }
    printf("test_skill_inject: PASS\n");
    return 0;
}
