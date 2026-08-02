#include "geistshell/grammar_mask.h"

#include <stdio.h>

static int fail(const char *m) {
    fprintf(stderr, "FAIL: %s\n", m);
    return 1;
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

    printf("test_grammar_mask ok\n");
    return 0;
}
