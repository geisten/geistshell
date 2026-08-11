/* #54: how to speak to a model is configuration, not a constant.
 *
 * Phase 12 measured what one universal prompt costs: the same constrained
 * decoder gave 9/9 parses for Gemma and 1/9 for BitNet b1.58, which returned a
 * lone backslash in one case. A model that was never shown a chat format
 * cannot be blamed for not answering in one. */

#include "geistshell/model_profile.h"

#include <stdio.h>
#include <string.h>

#define LIT(s) (sizeof(s) - 1u), (s)

static enum spg_status load(const size_t n, const char text[],
                            struct spg_model_profile *out) {
    struct spg_sexpr_token tokens[256];
    struct spg_sexpr_node  nodes[256];
    return spg_model_profile_load(n, text, 256u, tokens, 256u, nodes, out);
}

static int test_parse(void) {
    struct spg_model_profile p = {};
    if (load(LIT("(model_profile (name \"bitnet-full\") (arch \"bitnet-b1.58\")"
                 " (template llama3))"),
             &p) != SPG_OK) {
        return 1;
    }
    if (strcmp(p.name, "bitnet-full") != 0 ||
        strcmp(p.arch, "bitnet-b1.58") != 0 ||
        p.chat_template != SPG_TEMPLATE_LLAMA3 || !p.present) {
        return 1;
    }
    /* Unknown fields are ignored rather than rejected: a profile written for a
     * later version must still load. The earlier draft parsed constrained,
     * temperature and best_of, stored them, and never read them anywhere — a
     * config field nobody consumes reads like a promise. */
    if (load(LIT("(model_profile (name \"x\") (best_of 3) (temperature 8000))"),
             &p) != SPG_OK) {
        return 1;
    }
    if (strcmp(p.name, "x") != 0 || !p.present) {
        return 1;
    }
    return 0;
}

static int test_rejects(void) {
    struct spg_model_profile p = {};
    if (load(LIT("(policy (network_default deny))"), &p) != SPG_E_SCHEMA) {
        return 1;
    }
    /* An unrecognised template must NOT fall back to none: the run would
     * silently become the very experiment the profile exists to replace. */
    if (load(LIT("(model_profile (template chatml))"), &p) != SPG_E_SCHEMA) {
        return 1;
    }
    if (load(LIT("(model_profile (name \"x\")"), &p) == SPG_OK) {
        return 1;
    }
    return 0;
}

static int test_auto_detect(void) {
    if (spg_template_for_arch("gemma3") != SPG_TEMPLATE_GEMMA) {
        return 1;
    }
    if (spg_template_for_arch("llama") != SPG_TEMPLATE_LLAMA3) {
        return 1;
    }
    /* A base model with no chat format of its own gets NONE. Guessing a format
     * it was never trained on puts unfamiliar tokens in the prompt — worse
     * than sending none, and exactly the failure this ticket is fixing. */
    if (spg_template_for_arch("bitnet-b1.58") != SPG_TEMPLATE_NONE) {
        return 1;
    }
    if (spg_template_for_arch(nullptr) != SPG_TEMPLATE_NONE) {
        return 1;
    }
    return 0;
}

static int test_framing(void) {
    char   buf[512];
    size_t used = 0u;

    /* NONE must be byte-identical to the unframed prompt: every existing run
     * keeps its behaviour, and the phase-0 journal freeze depends on it. */
    if (spg_chat_frame(SPG_TEMPLATE_NONE, nullptr, LIT("(contract ...)"),
                       sizeof buf, buf, &used) != SPG_OK) {
        return 1;
    }
    if (strcmp(buf, "(contract ...)") != 0) {
        return 1;
    }

    if (spg_chat_frame(SPG_TEMPLATE_GEMMA, nullptr, LIT("ctx"), sizeof buf, buf,
                       &used) != SPG_OK) {
        return 1;
    }
    if (strcmp(buf, "<start_of_turn>user\nctx<end_of_turn>\n"
                    "<start_of_turn>model\n") != 0) {
        printf("  gemma: %s\n", buf);
        return 1;
    }

    /* Every template must END with the marker that opens the model's turn, or
     * the decoder continues the user's sentence instead of answering. */
    const enum spg_chat_template all[] = {
        SPG_TEMPLATE_GEMMA, SPG_TEMPLATE_LLAMA3, SPG_TEMPLATE_GENERIC};
    for (size_t i = 0u; i < sizeof all / sizeof all[0]; i += 1u) {
        if (spg_chat_frame(all[i], "sys", LIT("ctx"), sizeof buf, buf, &used) !=
            SPG_OK) {
            return 1;
        }
        if (strstr(buf, "ctx") == nullptr) {
            return 1;
        }
        const size_t n = strlen(buf);
        if (n == 0u || buf[n - 1u] != '\n') {
            printf("  %s does not end on a turn opener: %s\n",
                   spg_chat_template_to_string(all[i]), buf);
            return 1;
        }
    }

    /* Too small: no partial template escapes. A half-written chat format is
     * worse than none, because the model sees a broken marker it has to
     * interpret. */
    char   tight[8];
    size_t needed = 0u;
    if (spg_chat_frame(SPG_TEMPLATE_GEMMA, nullptr, LIT("ctx"), sizeof tight,
                       tight, &needed) != SPG_E_LIMIT) {
        return 1;
    }
    if (tight[0] != '\0') {
        return 1;
    }
    return 0;
}

int main(void) {
    const struct {
        const char *name;
        int (*fn)(void);
    } cases[] = {
        {"parse", test_parse},
        {"rejects", test_rejects},
        {"auto_detect", test_auto_detect},
        {"framing", test_framing},
    };
    int failures = 0;
    for (size_t i = 0u; i < sizeof cases / sizeof cases[0]; i += 1u) {
        if (cases[i].fn() != 0) {
            printf("test_model_profile: FAIL %s\n", cases[i].name);
            failures += 1;
        }
    }
    if (failures == 0) {
        printf("test_model_profile: PASS\n");
    }
    return failures == 0 ? 0 : 1;
}
