#include "geistshell/grammar_mask.h"

#include "geistshell/recommendation.h"

#include <stdio.h>

static int fail(const char *m) {
    fprintf(stderr, "FAIL: %s\n", m);
    return 1;
}

/* Concatenate (recommend (kind <name> + the scaffold, with a placeholder in
 * each free string slot, and assert the real parser accepts it as VALID. This
 * is stage 2's core claim — schema-valid by construction — checked without a
 * GGUF, straight against recommendation.c. */
static int check_scaffold_valid(enum spg_action_kind kind) {
    const struct spg_scaffold_seg *segs = nullptr;
    const size_t                   n     = spg_scaffold_for_kind(kind, &segs);
    if (n == 0u) {
        return fail("scaffold is empty for a known kind");
    }
    char   form[512];
    size_t len =
        (size_t) snprintf(form, sizeof form, "(recommend (kind %s",
                          spg_action_kind_to_string(kind));
    for (size_t i = 0u; i < n && len < sizeof form; i += 1u) {
        const char *seg = segs[i].literal != nullptr ? segs[i].literal : "value";
        len += (size_t) snprintf(form + len, sizeof form - len, "%s", seg);
    }

    struct spg_sexpr_token    toks[128];
    struct spg_sexpr_node     nodes[128];
    struct spg_recommendation rec;
    struct spg_recommendation_error err;
    const enum spg_status     st = spg_recommendation_parse(
        len, form, 128u, toks, 128u, nodes, &rec, &err);
    if (st != SPG_OK || rec.state != SPG_RECOMMENDATION_VALID) {
        fprintf(stderr, "not VALID (reason %d): %s\n", (int) rec.reject_reason,
                form);
        return fail("scaffold form did not parse as a valid recommendation");
    }
    if (rec.action_kind != kind) {
        return fail("scaffold parsed to the wrong kind");
    }
    return 0;
}

int main(void) {
    /* Live prefixes: empty + partial that extend a real kind name. */
    if (!spg_kind_prefix_ok("", "local")) {
        return fail("'local' is a live prefix of local_shell");
    }
    if (!spg_kind_prefix_ok("local", "_shell")) {
        return fail("local + _shell stays valid");
    }
    if (!spg_kind_prefix_ok("", "finish")) {
        return fail("'finish' is a live prefix");
    }
    if (!spg_kind_prefix_ok("memory", "_save")) {
        return fail("memory + _save stays valid");
    }
    /* Ambiguous stem shared by three kinds stays open. */
    if (!spg_kind_prefix_ok("", "memory")) {
        return fail("'memory' stem is a live prefix");
    }
    if (!spg_kind_prefix_ok("memory_", "read")) {
        return fail("memory_ + read stays valid");
    }
    /* Reaching a full name is itself a live prefix (len == name length). */
    if (!spg_kind_prefix_ok("", "finish") || !spg_kind_prefix_ok("finis", "h")) {
        return fail("completing exactly is still a live prefix");
    }

    /* Leading spaces from SentencePiece detokenization are ignored: the first
     * kind token reads as " local", must still match local_shell. */
    if (!spg_kind_prefix_ok("", " local")) {
        return fail("' local' (leading space) is a live prefix");
    }
    if (!spg_kind_prefix_ok(" ", "local")) {
        return fail("space then local is a live prefix");
    }
    if (!spg_kind_prefix_ok(" local", "_shell") ||
        !spg_kind_complete(" local_shell")) {
        return fail("leading-space name completes");
    }

    /* Rejections: unknown text and overshoot past a complete name. */
    if (spg_kind_prefix_ok("", "xyz")) {
        return fail("'xyz' is not any kind prefix");
    }
    if (spg_kind_prefix_ok("local_shell", "x")) {
        return fail("overshooting local_shell is rejected");
    }
    if (spg_kind_prefix_ok("memory_s", "x")) {
        return fail("memory_sx is not a prefix");
    }

    /* Completion: exact names only, no partials. */
    if (!spg_kind_complete("local_shell") || !spg_kind_complete("finish") ||
        !spg_kind_complete("memory_read") || !spg_kind_complete("ssh_auth_probe")) {
        return fail("full names are complete");
    }
    if (spg_kind_complete("memory") || spg_kind_complete("local") ||
        spg_kind_complete("") || spg_kind_complete("finishx")) {
        return fail("partials/overshoot are not complete");
    }

    /* text -> enum, tolerating the leading detok whitespace. */
    enum spg_action_kind k;
    if (!spg_kind_from_text(" simulator", &k) || k != SPG_ACTION_SIMULATOR) {
        return fail("' simulator' resolves to SPG_ACTION_SIMULATOR");
    }
    if (!spg_kind_from_text("\tfinish", &k) || k != SPG_ACTION_FINISH) {
        return fail("tab + finish resolves");
    }
    if (spg_kind_from_text("nope", &k)) {
        return fail("unknown kind does not resolve");
    }

    /* General choice mask over an arbitrary vocabulary (capability slot). */
    const char *const caps[] = {"sim.act", "build.run"};
    if (!spg_choice_prefix_ok(caps, 2u, "", "sim") ||
        !spg_choice_prefix_ok(caps, 2u, "sim", ".act") ||
        !spg_choice_prefix_ok(caps, 2u, "", " build") /* leading detok space */) {
        return fail("capability live prefixes");
    }
    if (spg_choice_prefix_ok(caps, 2u, "", "read") ||
        spg_choice_prefix_ok(caps, 2u, "sim.act", "x")) {
        return fail("non-candidate / overshoot rejected");
    }
    if (!spg_choice_complete(caps, 2u, "sim.act") ||
        !spg_choice_complete(caps, 2u, " build.run") ||
        spg_choice_complete(caps, 2u, "sim")) {
        return fail("capability completion");
    }

    /* Every kind's scaffold parses as a valid recommendation by construction. */
    const enum spg_action_kind kinds[] = {
        SPG_ACTION_LOCAL_SHELL, SPG_ACTION_SSH_AUTH_PROBE, SPG_ACTION_SIMULATOR,
        SPG_ACTION_MEMORY_SAVE, SPG_ACTION_MEMORY_DELETE, SPG_ACTION_MEMORY_READ,
        SPG_ACTION_FINISH,
    };
    for (size_t i = 0u; i < sizeof kinds / sizeof kinds[0]; i += 1u) {
        if (check_scaffold_valid(kinds[i]) != 0) {
            return 1;
        }
    }

    printf("test_grammar_mask ok\n");
    return 0;
}
