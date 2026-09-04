#include "geistshell/pref.h"

#include <stdio.h>
#include <string.h>

/* Append a slug-safe rendering of token to dst: [a-z0-9] kept (lowercased),
 * every other run collapses to a single '-'; no leading/trailing '-';
 * truncated to cap-1. Returns bytes written. Same rule as the skill/outcome
 * slugs in improve.c — kept local so pref.c has no dependency on improve. */
static size_t slugify_into(char *dst, const size_t cap, const char *token) {
    size_t w            = 0u;
    bool   pending_dash = false;
    for (const char *p = token; *p != '\0' && w + 1u < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (c >= 'A' && c <= 'Z') {
            c = (unsigned char)(c - 'A' + 'a');
        }
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            if (pending_dash && w > 0u && w + 1u < cap) {
                dst[w++] = '-';
            }
            pending_dash = false;
            if (w + 1u < cap) {
                dst[w++] = (char)c;
            }
        } else {
            pending_dash = w > 0u; /* no leading dash */
        }
    }
    dst[w] = '\0';
    return w;
}

bool spg_pref_should_write(const enum spg_pref_evidence evidence,
                           const size_t observed_count) {
    switch (evidence) {
    case SPG_PREF_EVIDENCE_ASSERTED:
        return false; /* a model self-assertion never writes — the whole point */
    case SPG_PREF_EVIDENCE_REPEATED:
        return observed_count >= 2u;
    case SPG_PREF_EVIDENCE_CORRECTION:
        return observed_count >= 1u;
    }
    return false;
}

static const char *evidence_name(const enum spg_pref_evidence e) {
    switch (e) {
    case SPG_PREF_EVIDENCE_ASSERTED:
        return "asserted";
    case SPG_PREF_EVIDENCE_REPEATED:
        return "repeated";
    case SPG_PREF_EVIDENCE_CORRECTION:
        return "correction";
    }
    return "unknown";
}

enum spg_status spg_pref_record(struct spg_mem_store *store, const char *key,
                                const char *value,
                                const enum spg_pref_evidence evidence,
                                const size_t observed_count, bool *wrote) {
    if (wrote != nullptr) {
        *wrote = false;
    }
    if (store == nullptr || key == nullptr || key[0] == '\0' ||
        value == nullptr || value[0] == '\0') {
        return SPG_E_INVALID_ARG;
    }
    if (!spg_pref_should_write(evidence, observed_count)) {
        return SPG_OK; /* declining to write is correct, not an error */
    }

    char         slug[SPG_MEM_SLUG_MAX + 1u];
    const size_t w = (size_t)snprintf(slug, sizeof slug, "%s", "pref-");
    if (slugify_into(slug + w, sizeof slug - w, key) == 0u) {
        return SPG_E_INVALID_ARG; /* a key that sanitises to nothing */
    }

    /* The description IS the injected line: "key=value". The body records the
     * provenance so the profile is auditable — WHY it was recorded, from what
     * evidence, never a model guess. */
    char description[SPG_MEM_DESC_MAX + 1u];
    (void)snprintf(description, sizeof description, "%s=%s", key, value);
    char body[512];
    (void)snprintf(body, sizeof body,
                   "User preference recorded from %s evidence (%zu "
                   "observation(s)): %s = %s. This shapes defaults and framing "
                   "only; it never changes what the policy permits.",
                   evidence_name(evidence), observed_count, key, value);

    const enum spg_status s = spg_mem_save(store, slug, description, body);
    if (s == SPG_OK && wrote != nullptr) {
        *wrote = true;
    }
    return s;
}

size_t spg_pref_render(struct spg_mem_store *store, const size_t budget_bytes,
                       const size_t dst_cap, char dst[]) {
    if (store == nullptr || dst == nullptr || dst_cap == 0u) {
        if (dst != nullptr && dst_cap > 0u) {
            dst[0] = '\0';
        }
        return 0u;
    }
    dst[0] = '\0';
    const size_t budget =
        budget_bytes == 0u || budget_bytes > SPG_MEM_DESC_MAX ? SPG_MEM_DESC_MAX
                                                              : budget_bytes;

    static char slugs[SPG_MEM_MAX_FILES][SPG_MEM_SLUG_MAX + 1u];
    size_t      count = 0u;
    /* SPG_E_LIMIT is fine: spg_mem_list writes the first cap slugs, which is
     * all a bounded profile line could show anyway. */
    const enum spg_status ls = spg_mem_list(store, SPG_MEM_MAX_FILES, slugs,
                                            &count);
    if (ls != SPG_OK && ls != SPG_E_LIMIT) {
        return 0u;
    }

    /* Assemble "(profile \"v1; v2; ...\")" bounded by budget. The values are
     * the pref descriptions ("key=value"). slugs come slug-sorted, so the line
     * is deterministic. */
    char   inner[SPG_MEM_DESC_MAX + 1u];
    size_t used  = 0u;
    bool   any   = false;
    for (size_t i = 0u; i < count; i += 1u) {
        if (strncmp(slugs[i], "pref-", 5u) != 0) {
            continue;
        }
        char   line[SPG_MEM_DESC_MAX + 1u];
        const size_t n = spg_mem_directive(store, slugs[i], 0u, sizeof line,
                                           line);
        if (n == 0u) {
            continue;
        }
        const size_t sep = any ? 2u : 0u; /* "; " */
        if (used + sep + n >= sizeof inner || used + sep + n > budget) {
            break; /* budget/buffer bound — the profile line never grows past it */
        }
        if (any) {
            inner[used++] = ';';
            inner[used++] = ' ';
        }
        memcpy(inner + used, line, n);
        used += n;
        any = true;
    }
    if (!any) {
        return 0u;
    }
    inner[used] = '\0';

    const int m = snprintf(dst, dst_cap, "(profile \"%s\")", inner);
    if (m < 0 || (size_t)m >= dst_cap) {
        dst[0] = '\0';
        return 0u;
    }
    return (size_t)m;
}
