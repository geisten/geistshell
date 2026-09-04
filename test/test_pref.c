/* #28: user-profile memory. The write-on-evidence rule and the
 * capability-invariance boundary — the two properties the ticket calls
 * load-bearing — plus the budgeted one-line render. */

/* mkdtemp is POSIX; -std=c23 defines __STRICT_ANSI__, so glibc hides it on
 * Linux unless a feature-test macro asks for it (macOS exposes it by
 * default). The same 200809L the other test programs declare. */
#define _POSIX_C_SOURCE 200809L

#include "geistshell/context.h"
#include "geistshell/pref.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* The pure decision: a model self-assertion never writes; a repeated choice
 * needs two observations; a correction is authoritative on the first. */
static int test_should_write(void) {
    /* model self-assertion — never, whatever the count */
    if (spg_pref_should_write(SPG_PREF_EVIDENCE_ASSERTED, 0u) ||
        spg_pref_should_write(SPG_PREF_EVIDENCE_ASSERTED, 5u)) {
        return 1;
    }
    /* repeated — only at 2+ */
    if (spg_pref_should_write(SPG_PREF_EVIDENCE_REPEATED, 0u) ||
        spg_pref_should_write(SPG_PREF_EVIDENCE_REPEATED, 1u) ||
        !spg_pref_should_write(SPG_PREF_EVIDENCE_REPEATED, 2u) ||
        !spg_pref_should_write(SPG_PREF_EVIDENCE_REPEATED, 9u)) {
        return 1;
    }
    /* correction — authoritative from 1 */
    if (spg_pref_should_write(SPG_PREF_EVIDENCE_CORRECTION, 0u) ||
        !spg_pref_should_write(SPG_PREF_EVIDENCE_CORRECTION, 1u)) {
        return 1;
    }
    return 0;
}

static int test_record_and_render(void) {
    char dir[] = "/tmp/spg_pref_XXXXXX";
    if (mkdtemp(dir) == nullptr) {
        return 1;
    }
    struct spg_mem_store store;
    if (spg_mem_store_open(&store, dir) != SPG_OK) {
        return 1;
    }

    /* A model self-assertion writes nothing and touches no file. */
    bool wrote = true;
    if (spg_pref_record(&store, "editor", "vim", SPG_PREF_EVIDENCE_ASSERTED, 3u,
                        &wrote) != SPG_OK ||
        wrote) {
        return 1;
    }
    char line[SPG_MEM_DESC_MAX + 32u];
    if (spg_pref_render(&store, 0u, sizeof line, line) != 0u) {
        return 1; /* nothing recorded -> no profile line */
    }

    /* A repeated choice below threshold still writes nothing. */
    if (spg_pref_record(&store, "editor", "vim", SPG_PREF_EVIDENCE_REPEATED, 1u,
                        &wrote) != SPG_OK ||
        wrote) {
        return 1;
    }

    /* At the threshold it writes, and the profile line shows it. */
    if (spg_pref_record(&store, "editor", "vim", SPG_PREF_EVIDENCE_REPEATED, 2u,
                        &wrote) != SPG_OK ||
        !wrote) {
        return 1;
    }
    if (spg_pref_record(&store, "units", "metric",
                        SPG_PREF_EVIDENCE_CORRECTION, 1u, &wrote) != SPG_OK ||
        !wrote) {
        return 1;
    }
    const size_t n = spg_pref_render(&store, 0u, sizeof line, line);
    if (n == 0u || strcmp(line, "(profile \"editor=vim; units=metric\")") != 0) {
        fprintf(stderr, "  profile: %s\n", line);
        return 1;
    }

    /* Context-invariance: the line stays ONE line as the profile fills, and
     * the budget is a hard cap — a tiny budget yields nothing, never a
     * truncated half-line. */
    if (spg_pref_render(&store, 8u, sizeof line, line) != 0u || line[0] != '\0') {
        return 1;
    }

    /* Invalid inputs: empty key/value, null store. */
    if (spg_pref_record(nullptr, "k", "v", SPG_PREF_EVIDENCE_CORRECTION, 1u,
                        &wrote) != SPG_E_INVALID_ARG ||
        spg_pref_record(&store, "", "v", SPG_PREF_EVIDENCE_CORRECTION, 1u,
                        &wrote) != SPG_E_INVALID_ARG ||
        spg_pref_record(&store, "k", "", SPG_PREF_EVIDENCE_CORRECTION, 1u,
                        &wrote) != SPG_E_INVALID_ARG) {
        return 1;
    }
    return 0;
}

/* The capability-invariance boundary: a profile line is CONTEXT. Rendering a
 * context with and without it changes only the added line — the profile never
 * appears anywhere the policy is decided, so it cannot widen capability. Here
 * we prove the structural half: the line lands in the rendered context, and
 * the rest of the context is byte-identical to the no-profile render. */
static int test_context_is_framing_only(void) {
    struct spg_context_sources with = {.user_profile =
                                            "(profile \"editor=vim\")"};
    struct spg_context_sources without = {0};
    struct spg_context_view    view    = {};
    static char                a[16384];
    static char                b[16384];
    size_t                     na = 0u, nb = 0u;
    if (spg_context_render(&without, &view, sizeof a, a, &na) != SPG_OK ||
        spg_context_render(&with, &view, sizeof b, b, &nb) != SPG_OK) {
        return 1;
    }
    /* the profile line is present with, absent without */
    if (strstr(b, "(profile \"editor=vim\")") == nullptr ||
        strstr(a, "(profile") != nullptr) {
        return 1;
    }
    /* removing exactly that line from the with-render yields the without-render:
     * the profile ADDS a framing line and changes nothing else */
    const char *line = strstr(b, "(profile \"editor=vim\")\n");
    if (line == nullptr) {
        return 1;
    }
    static char stripped[16384];
    const size_t head = (size_t)(line - b);
    const size_t skip = strlen("(profile \"editor=vim\")\n");
    memcpy(stripped, b, head);
    (void)strcpy(stripped + head, line + skip);
    if (strcmp(stripped, a) != 0) {
        return 1;
    }
    return 0;
}

int main(void) {
    if (test_should_write() != 0) {
        fprintf(stderr, "test_should_write failed\n");
        return 1;
    }
    if (test_record_and_render() != 0) {
        fprintf(stderr, "test_record_and_render failed\n");
        return 1;
    }
    if (test_context_is_framing_only() != 0) {
        fprintf(stderr, "test_context_is_framing_only failed\n");
        return 1;
    }
    printf("test_pref: PASS\n");
    return 0;
}
