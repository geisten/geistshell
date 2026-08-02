#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#    define _DARWIN_C_SOURCE 1
#endif

#include "geistshell/mem_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Each test opens a fresh temp directory so they are independent. */
static int open_temp(struct spg_mem_store *store, char dir[static 64]) {
    memcpy(dir, "/tmp/spg_mem_XXXXXX", 20u);
    if (mkdtemp(dir) == nullptr) {
        return 1;
    }
    return spg_mem_store_open(store, dir) == SPG_OK ? 0 : 1;
}

static int test_slug_validation(void) {
    if (!spg_mem_slug_valid("auth-flow") || !spg_mem_slug_valid("a") ||
        !spg_mem_slug_valid("x9-y")) {
        return 1;
    }
    /* Path traversal, separators, dots, uppercase, empty are all rejected. */
    if (spg_mem_slug_valid("../evil") || spg_mem_slug_valid("a/b") ||
        spg_mem_slug_valid("a.b") || spg_mem_slug_valid("Abc") ||
        spg_mem_slug_valid("") || spg_mem_slug_valid(nullptr)) {
        return 1;
    }
    char big[SPG_MEM_SLUG_MAX + 2u];
    memset(big, 'a', sizeof big - 1u);
    big[sizeof big - 1u] = '\0';
    return spg_mem_slug_valid(big) ? 1 : 0; /* overlong rejected */
}

static int test_save_read_roundtrip(void) {
    struct spg_mem_store store;
    char                 dir[64];
    if (open_temp(&store, dir) != 0) {
        return 1;
    }
    if (spg_mem_save(&store, "note", "a test hook", "Hello body.\n") != SPG_OK) {
        return 1;
    }
    char   out[256];
    size_t required = 0u;
    if (spg_mem_read(&store, "note", sizeof out, out, &required) != SPG_OK) {
        return 1;
    }
    /* Content holds the frontmatter and body. */
    if (strstr(out, "name: note") == nullptr ||
        strstr(out, "description: a test hook") == nullptr ||
        strstr(out, "Hello body.") == nullptr) {
        return 1;
    }
    return required == strlen(out) ? 0 : 1;
}

/* P6 (docs/LEARNING.md): the one-line directive for slug-triggered
 * auto-injection — description only, budget-capped, one slot. */
static int test_directive(void) {
    struct spg_mem_store store;
    char                 dir[64];
    if (open_temp(&store, dir) != 0) {
        return 1;
    }
    if (spg_mem_save(&store, "lesson-rejected", "Emit one valid form.",
                     "long body text that must NOT be injected") != SPG_OK) {
        return 1;
    }
    char   out[256];
    /* the directive is the description, not the body */
    size_t n = spg_mem_directive(&store, "lesson-rejected", 0u, sizeof out, out);
    if (n != strlen("Emit one valid form.") ||
        strcmp(out, "Emit one valid form.") != 0) {
        return 1;
    }
    /* a budget smaller than the description injects nothing (over budget) */
    if (spg_mem_directive(&store, "lesson-rejected", 5u, sizeof out, out) != 0u ||
        out[0] != '\0') {
        return 1;
    }
    /* a budget that fits still returns it */
    if (spg_mem_directive(&store, "lesson-rejected", 64u, sizeof out, out) == 0u) {
        return 1;
    }
    /* an unknown slug injects nothing */
    if (spg_mem_directive(&store, "lesson-absent", 0u, sizeof out, out) != 0u) {
        return 1;
    }
    /* null args are rejected */
    if (spg_mem_directive(nullptr, "x", 0u, sizeof out, out) != 0u ||
        spg_mem_directive(&store, nullptr, 0u, sizeof out, out) != 0u) {
        return 1;
    }
    return 0;
}

static int test_upsert_overwrites(void) {
    struct spg_mem_store store;
    char                 dir[64];
    if (open_temp(&store, dir) != 0) {
        return 1;
    }
    if (spg_mem_save(&store, "k", "first", "v1") != SPG_OK ||
        spg_mem_save(&store, "k", "second", "v2") != SPG_OK) {
        return 1;
    }
    char out[256];
    if (spg_mem_read(&store, "k", sizeof out, out, nullptr) != SPG_OK) {
        return 1;
    }
    /* Only the second value/description survives; not duplicated. */
    if (strstr(out, "v2") == nullptr || strstr(out, "v1") != nullptr ||
        strstr(out, "description: second") == nullptr) {
        return 1;
    }
    size_t count = 0u;
    char   slugs[4][SPG_MEM_SLUG_MAX + 1u];
    if (spg_mem_list(&store, 4u, slugs, &count) != SPG_OK || count != 1u) {
        return 1;
    }
    return 0;
}

static int test_delete(void) {
    struct spg_mem_store store;
    char                 dir[64];
    if (open_temp(&store, dir) != 0) {
        return 1;
    }
    char out[64];
    if (spg_mem_delete(&store, "ghost") != SPG_E_NOT_FOUND) {
        return 1;
    }
    if (spg_mem_save(&store, "tmp", "d", "b") != SPG_OK ||
        spg_mem_delete(&store, "tmp") != SPG_OK) {
        return 1;
    }
    return spg_mem_read(&store, "tmp", sizeof out, out, nullptr) ==
                   SPG_E_NOT_FOUND
               ? 0
               : 1;
}

static int test_index(void) {
    struct spg_mem_store store;
    char                 dir[64];
    if (open_temp(&store, dir) != 0) {
        return 1;
    }
    /* Save aaa first, then bbb: recency ranking must list bbb (newer) before
     * aaa, which differs from alphabetical order. */
    if (spg_mem_save(&store, "aaa", "older hook", "x") != SPG_OK ||
        spg_mem_save(&store, "bbb", "newer hook", "y") != SPG_OK) {
        return 1;
    }
    char   idx[512];
    bool   truncated = true;
    size_t required  = 0u;
    if (spg_mem_index(&store, sizeof idx, idx, &required, &truncated) !=
        SPG_OK) {
        return 1;
    }
    const char *a = strstr(idx, "- aaa: older hook");
    const char *b = strstr(idx, "- bbb: newer hook");
    if (a == nullptr || b == nullptr || b > a || truncated) {
        return 1; /* bbb (newer) must come before aaa */
    }
    /* Re-saving aaa bumps it to the most recent, so it leads now. */
    if (spg_mem_save(&store, "aaa", "older hook", "x2") != SPG_OK ||
        spg_mem_index(&store, sizeof idx, idx, &required, &truncated) !=
            SPG_OK) {
        return 1;
    }
    if (strstr(idx, "- aaa:") > strstr(idx, "- bbb:")) {
        return 1; /* aaa now first after the re-save */
    }
    return required == strlen(idx) ? 0 : 1;
}

static int test_limits(void) {
    struct spg_mem_store store;
    char                 dir[64];
    if (open_temp(&store, dir) != 0) {
        return 1;
    }
    /* Oversize description / body. */
    char big_desc[SPG_MEM_DESC_MAX + 2u];
    memset(big_desc, 'd', sizeof big_desc - 1u);
    big_desc[sizeof big_desc - 1u] = '\0';
    if (spg_mem_save(&store, "k", big_desc, "b") != SPG_E_LIMIT) {
        return 1;
    }
    /* Newline in description is rejected. */
    if (spg_mem_save(&store, "k", "a\nb", "b") != SPG_E_INVALID_ARG) {
        return 1;
    }
    /* read into a too-small buffer truncates and reports the full size. */
    if (spg_mem_save(&store, "k", "d", "0123456789") != SPG_OK) {
        return 1;
    }
    char   small[8];
    size_t required = 0u;
    if (spg_mem_read(&store, "k", sizeof small, small, &required) !=
        SPG_E_LIMIT) {
        return 1;
    }
    return (required > sizeof small && small[sizeof small - 1u] == '\0') ? 0 : 1;
}

static int test_invalid_args(void) {
    struct spg_mem_store store;
    char                 dir[64];
    if (open_temp(&store, dir) != 0) {
        return 1;
    }
    char out[64];
    if (spg_mem_save(&store, "../x", "d", "b") != SPG_E_INVALID_ARG ||
        spg_mem_read(&store, "a/b", sizeof out, out, nullptr) !=
            SPG_E_INVALID_ARG) {
        return 1;
    }
    return 0;
}

/* The index is served from a per-store cache that must be invalidated on every
 * save and delete; a stale cache would silently return outdated context. Read
 * first (populating the cache), then mutate, then read again and require the
 * change to be reflected. Repeated reads must be byte-identical. */
static int test_index_cache_invalidation(void) {
    struct spg_mem_store store;
    char                 dir[64];
    if (open_temp(&store, dir) != 0) {
        return 1;
    }
    if (spg_mem_save(&store, "alpha", "first", "a") != SPG_OK) {
        return 1;
    }
    char   idx1[512];
    char   idx2[512];
    size_t required = 0u;
    bool   trunc    = false;
    /* Populate the cache, then read again: identical bytes and length. */
    if (spg_mem_index(&store, sizeof idx1, idx1, &required, &trunc) != SPG_OK ||
        spg_mem_index(&store, sizeof idx2, idx2, &required, &trunc) != SPG_OK ||
        strcmp(idx1, idx2) != 0 || required != strlen(idx1)) {
        return 1;
    }
    if (strstr(idx1, "- alpha: first") == nullptr) {
        return 1;
    }
    /* A save after a cached read must surface in the next read. */
    if (spg_mem_save(&store, "beta", "second", "b") != SPG_OK ||
        spg_mem_index(&store, sizeof idx2, idx2, &required, &trunc) != SPG_OK ||
        strstr(idx2, "- beta: second") == nullptr) {
        return 1;
    }
    /* A delete after a cached read must surface too. */
    if (spg_mem_delete(&store, "beta") != SPG_OK ||
        spg_mem_index(&store, sizeof idx2, idx2, &required, &trunc) != SPG_OK ||
        strstr(idx2, "beta") != nullptr) {
        return 1;
    }
    return 0;
}

int main(void) {
    if (test_slug_validation() != 0) {
        fprintf(stderr, "test_slug_validation failed\n");
        return 1;
    }
    if (test_save_read_roundtrip() != 0) {
        fprintf(stderr, "test_save_read_roundtrip failed\n");
        return 1;
    }
    if (test_directive() != 0) {
        fprintf(stderr, "test_directive failed\n");
        return 1;
    }
    if (test_upsert_overwrites() != 0) {
        fprintf(stderr, "test_upsert_overwrites failed\n");
        return 1;
    }
    if (test_delete() != 0) {
        fprintf(stderr, "test_delete failed\n");
        return 1;
    }
    if (test_index() != 0) {
        fprintf(stderr, "test_index failed\n");
        return 1;
    }
    if (test_index_cache_invalidation() != 0) {
        fprintf(stderr, "test_index_cache_invalidation failed\n");
        return 1;
    }
    if (test_limits() != 0) {
        fprintf(stderr, "test_limits failed\n");
        return 1;
    }
    if (test_invalid_args() != 0) {
        fprintf(stderr, "test_invalid_args failed\n");
        return 1;
    }
    return 0;
}
