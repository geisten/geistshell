#include "geistshell/grammar_mask.h"

#include "geistshell/recommendation.h"

#include <stdio.h>
#include <string.h>

static int fail(const char *m) {
    fprintf(stderr, "FAIL: %s\n", m);
    return 1;
}

/* The capability mask table both `agent --constrained` and `eval --constrained`
 * build from (#51). Pure: policy config in, borrowed names out, no engine. */
static int check_caps_from_policy(void) {
    /* Offsets into this text are what the policy config stores as name spans. */
    static const char text[] = "sim.act build.run off.cap mem.rw";
    struct spg_policy_config policy = {
        .capability_count = 4u,
        .capabilities =
            {
                {.name    = {.offset = 0u, .length = 7u}, /* sim.act   */
                 .kind    = SPG_POLICY_CAP_SIMULATOR,
                 .enabled = true},
                {.name    = {.offset = 8u, .length = 9u}, /* build.run */
                 .kind    = SPG_POLICY_CAP_LOCAL_SHELL,
                 .enabled = true},
                {.name    = {.offset = 18u, .length = 7u}, /* off.cap  */
                 .kind    = SPG_POLICY_CAP_LOCAL_SHELL,
                 .enabled = false},
                {.name    = {.offset = 26u, .length = 6u}, /* mem.rw   */
                 .kind    = SPG_POLICY_CAP_MEMORY,
                 .enabled = true},
            },
    };

    struct spg_model_capability out[SPG_MODEL_CAPABILITY_MAX];
    char                        names[128];
    size_t n = spg_model_capabilities_from_policy(&policy, sizeof text - 1u,
                                                  text, sizeof names, names,
                                                  sizeof out / sizeof out[0],
                                                  out);
    /* 1 simulator + 1 local_shell + 3 memory kinds; the disabled cap is gone. */
    if (n != 5u) {
        return fail("capability expansion count");
    }
    if (out[0].kind != SPG_ACTION_SIMULATOR ||
        strcmp(out[0].name, "sim.act") != 0) {
        return fail("simulator capability");
    }
    if (out[1].kind != SPG_ACTION_LOCAL_SHELL ||
        strcmp(out[1].name, "build.run") != 0) {
        return fail("local_shell capability");
    }
    if (out[2].kind != SPG_ACTION_MEMORY_SAVE ||
        out[3].kind != SPG_ACTION_MEMORY_DELETE ||
        out[4].kind != SPG_ACTION_MEMORY_READ) {
        return fail("memory capability expands per kind");
    }
    /* One capability, one stored name: the three memory entries share it. */
    if (out[2].name != out[3].name || out[3].name != out[4].name ||
        strcmp(out[2].name, "mem.rw") != 0) {
        return fail("memory entries share one borrowed name");
    }
    for (size_t i = 0u; i < n; i += 1u) {
        if (strcmp(out[i].name, "off.cap") == 0) {
            return fail("disabled capability leaked into the mask");
        }
    }

    /* A name buffer too small truncates to a NARROWER mask, never a malformed
     * one — the decoder then free-decodes that slot and the policy gate still
     * decides. */
    char tiny[8]; /* fits "sim.act" and nothing more */
    n = spg_model_capabilities_from_policy(&policy, sizeof text - 1u, text,
                                           sizeof tiny, tiny,
                                           sizeof out / sizeof out[0], out);
    if (n != 1u || strcmp(out[0].name, "sim.act") != 0) {
        return fail("name-buffer overflow narrows the mask");
    }

    if (spg_model_capabilities_from_policy(nullptr, sizeof text - 1u, text,
                                           sizeof names, names, 8u, out) != 0u) {
        return fail("null policy rejected");
    }
    return 0;
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

    /* The verbs that were missing from ALL_KINDS while being parseable and
     * scaffolded: the constrained decoder could not TYPE them, so every
     * --constrained run looked like a model that never reached the actuator.
     * These lines are the regression guard against the list drifting again. */
    if (!spg_kind_prefix_ok("", "device") ||
        !spg_kind_prefix_ok("device", "_write") ||
        !spg_kind_complete("device_write")) {
        return fail("device_write must be typable under the mask");
    }
    if (!spg_kind_prefix_ok("", "machine") ||
        !spg_kind_complete("machine_pause_process") ||
        !spg_kind_complete("machine_resume_process")) {
        return fail("machine kinds must be typable under the mask");
    }
    enum spg_action_kind parsed_kind;
    if (!spg_kind_from_text("device_write", &parsed_kind) ||
        parsed_kind != SPG_ACTION_DEVICE_WRITE) {
        return fail("device_write must round-trip through the mask");
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

    if (check_caps_from_policy() != 0) {
        return 1;
    }

    /* #124/#57 PMI selector: null baseline is plain argmax; a baseline shifts
     * the winner by the request-free prior; ties keep the lowest index. */
    {
        const float logits[3] = {1.0f, 3.0f, 2.0f};
        if (spg_pmi_pick(3u, logits, nullptr) != 1u) {
            return fail("null baseline must be raw argmax");
        }
        /* candidate 1 is common in pretraining (high baseline), so once its
         * prior is subtracted the request-driven winner becomes candidate 2. */
        const float baseline[3] = {0.0f, 2.5f, 0.0f};
        if (spg_pmi_pick(3u, logits, baseline) != 2u) {
            return fail("calibration must subtract the pretraining prior");
        }
        const float tie[2] = {5.0f, 5.0f};
        if (spg_pmi_pick(2u, tie, nullptr) != 0u ||
            spg_pmi_pick(0u, tie, nullptr) != 0u) {
            return fail("ties keep the lowest index; n=0 returns 0");
        }
    }

    printf("test_grammar_mask ok\n");
    return 0;
}
