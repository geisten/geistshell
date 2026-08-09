#include "geistshell/fixture.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fail(const char *m) {
    fprintf(stderr, "FAIL: %s\n", m);
    return 1;
}

/* Everything this test creates lives under one relative root, so a bug here
 * cannot reach outside the build tree even if the guards it is testing are the
 * thing that is broken. */
#define ROOT "build/test-fixture"

static bool exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static bool is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (f == nullptr) {
        return false;
    }
    const size_t n  = strlen(text);
    const bool   ok = fwrite(text, 1u, n, f) == n;
    return fclose(f) == 0 && ok;
}

/* Read at most cap-1 bytes; empty string when the file is absent. */
static void read_file_into(const char *path, size_t cap, char out[]) {
    out[0]  = '\0';
    FILE *f = fopen(path, "rb");
    if (f == nullptr) {
        return;
    }
    const size_t n = fread(out, 1u, cap - 1u, f);
    out[n]         = '\0';
    (void)fclose(f);
}

/* ---- spg_fixture_sample_dir --------------------------------------------- */

static int test_sample_dir(void) {
    char path[256];

    if (spg_fixture_sample_dir(ROOT, "mem-recall", 3u, sizeof path, path) !=
            SPG_OK ||
        strcmp(path, ROOT "/mem-recall-3") != 0) {
        return fail("sample dir path");
    }
    /* Every accepted byte class, so the validator cannot be tightened by
     * accident into rejecting a name a real suite would use. */
    if (spg_fixture_sample_dir(ROOT, "Case_9.v-2", 0u, sizeof path, path) !=
        SPG_OK) {
        return fail("alphanumeric . _ - are accepted in a case name");
    }

    /* A suite file must not be able to steer the path. These are the bytes
     * that would let it. */
    static const char *const evil[] = {
        "..", "a/b", "/abs", "../x", "x/..", ".", "", "a b", "a\\b", "a;rm",
    };
    for (size_t i = 0u; i < sizeof evil / sizeof evil[0]; i += 1u) {
        if (spg_fixture_sample_dir(ROOT, evil[i], 0u, sizeof path, path) ==
            SPG_OK) {
            fprintf(stderr, "accepted case name: '%s'\n", evil[i]);
            return fail("unsafe case name accepted");
        }
    }
    if (spg_fixture_sample_dir(ROOT, nullptr, 0u, sizeof path, path) !=
            SPG_E_INVALID_ARG ||
        spg_fixture_sample_dir(nullptr, "x", 0u, sizeof path, path) !=
            SPG_E_INVALID_ARG ||
        spg_fixture_sample_dir("", "x", 0u, sizeof path, path) !=
            SPG_E_INVALID_ARG ||
        spg_fixture_sample_dir(ROOT, "x", 0u, sizeof path, nullptr) !=
            SPG_E_INVALID_ARG) {
        return fail("null/empty arguments rejected");
    }
    /* A root that could climb out is refused even with a safe case name. */
    if (spg_fixture_sample_dir("build/../..", "x", 0u, sizeof path, path) !=
        SPG_E_INVALID_ARG) {
        return fail("root containing .. rejected");
    }
    /* Truncation is an error, never a shorter path that points elsewhere. */
    char tiny[8];
    if (spg_fixture_sample_dir(ROOT, "long-case-name", 0u, sizeof tiny, tiny) !=
            SPG_E_LIMIT ||
        tiny[0] != '\0') {
        return fail("overflow returns LIMIT and clears the output");
    }
    return 0;
}

/* ---- spg_fixture_reset --------------------------------------------------- */

static int test_reset_guards(void) {
    /* Paths that must never be handed to rm -rf. If any of these returned OK
     * the test itself would have deleted something outside the build tree, so
     * the assertion is the whole point of the function existing. */
    static const char *const forbidden[] = {
        "/",  "/tmp", "..",       "../build", ".",     "build",
        "",   "a//b", "build/..", "build/../etc",      "build/x/",
    };
    for (size_t i = 0u; i < sizeof forbidden / sizeof forbidden[0]; i += 1u) {
        if (spg_fixture_reset(forbidden[i]) != SPG_E_INVALID_ARG) {
            fprintf(stderr, "accepted delete target: '%s'\n", forbidden[i]);
            return fail("unsafe delete target accepted");
        }
    }
    if (spg_fixture_reset(nullptr) != SPG_E_INVALID_ARG) {
        return fail("null delete target rejected");
    }
    /* A name that merely CONTAINS dots is fine — the guard is per component,
     * not a substring search, or a legitimate case name would be unusable. */
    if (spg_fixture_reset(ROOT "/my..case-0") != SPG_OK) {
        return fail("a name containing '..' inside a component is allowed");
    }
    if (!is_dir(ROOT "/my..case-0")) {
        return fail("reset created the directory");
    }
    return 0;
}

static int test_reset_clears(void) {
    const char *dir = ROOT "/clear-0";
    if (spg_fixture_reset(dir) != SPG_OK || !is_dir(dir)) {
        return fail("reset creates a missing directory");
    }
    if (!write_file(ROOT "/clear-0/leftover.txt", "from the previous sample")) {
        return fail("test setup: write leftover");
    }
    if (!exists(ROOT "/clear-0/leftover.txt")) {
        return fail("test setup: leftover exists");
    }
    /* The defect this whole module exists for: a second sample must not see
     * the first sample's mutation. */
    if (spg_fixture_reset(dir) != SPG_OK) {
        return fail("reset on an existing directory");
    }
    if (exists(ROOT "/clear-0/leftover.txt")) {
        return fail("reset left the previous sample's file behind");
    }
    if (!is_dir(dir)) {
        return fail("reset recreated the directory");
    }
    return 0;
}

/* ---- spg_fixture_copy_into ----------------------------------------------- */

static int test_copy_into(void) {
    const char *src = ROOT "/src-0";
    const char *dst = ROOT "/dst-0";
    if (spg_fixture_reset(src) != SPG_OK || spg_fixture_reset(dst) != SPG_OK) {
        return fail("test setup: reset src/dst");
    }
    if (!write_file(ROOT "/src-0/report.md", "seven\n")) {
        return fail("test setup: write fixture file");
    }

    if (spg_fixture_copy_into(dst, src) != SPG_OK) {
        return fail("copy_into");
    }
    /* CONTENTS, not the directory itself — otherwise a second overlay would
     * nest instead of merge. */
    if (!exists(ROOT "/dst-0/report.md")) {
        return fail("copy_into copies the contents");
    }
    if (exists(ROOT "/dst-0/src-0")) {
        return fail("copy_into must not nest the source directory");
    }

    /* Overlay order: a later source wins, which is how the shared mind-palace
     * is layered on top of a case fixture. */
    const char *src2 = ROOT "/src2-0";
    if (spg_fixture_reset(src2) != SPG_OK ||
        !write_file(ROOT "/src2-0/report.md", "overlaid\n") ||
        !write_file(ROOT "/src2-0/extra.md", "new\n")) {
        return fail("test setup: second source");
    }
    if (spg_fixture_copy_into(dst, src2) != SPG_OK) {
        return fail("overlay copy");
    }
    char buf[64];
    read_file_into(ROOT "/dst-0/report.md", sizeof buf, buf);
    if (strcmp(buf, "overlaid\n") != 0) {
        return fail("a later overlay wins");
    }
    if (!exists(ROOT "/dst-0/extra.md")) {
        return fail("overlay adds new files");
    }

    /* "no fixture declared" is not an error. */
    if (spg_fixture_copy_into(dst, nullptr) != SPG_OK ||
        spg_fixture_copy_into(dst, "") != SPG_OK) {
        return fail("a null/empty source is a no-op");
    }
    if (spg_fixture_copy_into(dst, ROOT "/does-not-exist") != SPG_E_NOT_FOUND) {
        return fail("a missing source is NOT_FOUND");
    }
    if (spg_fixture_copy_into(ROOT "/no-such-dst", src) != SPG_E_INVALID_ARG) {
        return fail("a missing destination is INVALID_ARG");
    }
    if (spg_fixture_copy_into(nullptr, src) != SPG_E_INVALID_ARG) {
        return fail("a null destination is INVALID_ARG");
    }
    /* A file where a directory is expected must not be copied over. */
    if (!write_file(ROOT "/plain.txt", "x")) {
        return fail("test setup: plain file");
    }
    if (spg_fixture_copy_into(dst, ROOT "/plain.txt") != SPG_E_NOT_FOUND) {
        return fail("a non-directory source is NOT_FOUND");
    }
    return 0;
}

/* The composed sequence the harness actually runs, twice, asserting the second
 * pass cannot see the first pass's mutation. */
static int test_sample_cycle_is_pristine(void) {
    const char *fixture = ROOT "/fixt-0";
    if (spg_fixture_reset(fixture) != SPG_OK ||
        !write_file(ROOT "/fixt-0/report.md", "line\n")) {
        return fail("test setup: fixture");
    }
    for (size_t sample = 0u; sample < 2u; sample += 1u) {
        char dir[256];
        if (spg_fixture_sample_dir(ROOT, "cycle", sample, sizeof dir, dir) !=
                SPG_OK ||
            spg_fixture_reset(dir) != SPG_OK ||
            spg_fixture_copy_into(dir, fixture) != SPG_OK) {
            return fail("sample cycle");
        }
        char probe[256];
        (void)snprintf(probe, sizeof probe, "%s/mutation.txt", dir);
        if (exists(probe)) {
            return fail("a sample saw a previous sample's mutation");
        }
        if (!write_file(probe, "the case mutated its workdir")) {
            return fail("test setup: mutate");
        }
        /* Re-running the SAME sample index must also start clean, which is
         * what makes a whole-suite rerun reproducible. */
        if (spg_fixture_reset(dir) != SPG_OK ||
            spg_fixture_copy_into(dir, fixture) != SPG_OK) {
            return fail("re-running one sample index");
        }
        if (exists(probe)) {
            return fail("re-running a sample index left the mutation");
        }
        char buf[64];
        (void)snprintf(probe, sizeof probe, "%s/report.md", dir);
        read_file_into(probe, sizeof buf, buf);
        if (strcmp(buf, "line\n") != 0) {
            return fail("the fixture content is restored each time");
        }
    }
    return 0;
}

int main(void) {
    if (spg_fixture_reset(ROOT "/self-0") != SPG_OK) {
        return fail("test setup: root usable");
    }
    struct {
        const char *name;
        int (*fn)(void);
    } cases[] = {
        {"sample_dir", test_sample_dir},
        {"reset_guards", test_reset_guards},
        {"reset_clears", test_reset_clears},
        {"copy_into", test_copy_into},
        {"sample_cycle_is_pristine", test_sample_cycle_is_pristine},
    };
    for (size_t i = 0u; i < sizeof cases / sizeof cases[0]; i += 1u) {
        if (cases[i].fn() != 0) {
            fprintf(stderr, "  in %s\n", cases[i].name);
            return 1;
        }
    }
    printf("test_fixture ok\n");
    return 0;
}
