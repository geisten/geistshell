#define _POSIX_C_SOURCE 200809L

#include "geistshell/fixture.h"

#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

/* Run argv to completion with the inherited environment. True only on a clean
 * exit 0 — a signal death or a non-zero status is a failure, never ignored. */
static bool run_to_completion(const char *const argv[]) {
    pid_t     pid = 0;
    const int rc  = posix_spawnp(&pid, argv[0], nullptr, nullptr,
                                 (char *const *)argv, environ);
    if (rc != 0) {
        return false;
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        /* interrupted: keep waiting rather than leaking the child */
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* A case name may only contain bytes that cannot steer a path: no '/', so no
 * escape into another directory.
 *
 * An all-dots name ("." or "..") is refused too. The "-<sample>" suffix already
 * makes it a harmless component ("..-0"), but relying on that would make path
 * safety depend on the naming format — and the next person to change the format
 * would not know they were holding it up. */
static bool name_is_safe(const char *name) {
    if (name == nullptr || name[0] == '\0') {
        return false;
    }
    bool all_dots = true;
    for (size_t i = 0u; name[i] != '\0'; i += 1u) {
        const char c = name[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                        c == '-';
        if (!ok) {
            return false;
        }
        all_dots = all_dots && c == '.';
    }
    return !all_dots;
}

enum spg_status spg_fixture_sample_dir(const char *root, const char *case_name,
                                       const size_t sample, const size_t cap,
                                       char out[]) {
    if (root == nullptr || root[0] == '\0' || out == nullptr || cap == 0u ||
        !name_is_safe(case_name) || strstr(root, "..") != nullptr) {
        return SPG_E_INVALID_ARG;
    }
    const int n =
        snprintf(out, cap, "%s/%s-%zu", root, case_name, sample);
    if (n < 0 || (size_t)n >= cap) {
        if (cap > 0u) {
            out[0] = '\0';
        }
        return SPG_E_LIMIT;
    }
    return SPG_OK;
}

/* Relative, nested at least one level, and free of any ".." component. The
 * component test is deliberate: rejecting the substring ".." would also reject
 * a legitimate "my..case", while allowing the substring would accept "a/../..".
 */
static bool dir_is_deletable(const char *dir) {
    if (dir == nullptr || dir[0] == '\0' || dir[0] == '/') {
        return false;
    }
    bool   nested = false;
    size_t start  = 0u;
    for (size_t i = 0u;; i += 1u) {
        if (dir[i] != '/' && dir[i] != '\0') {
            continue;
        }
        const size_t len = i - start;
        if (len == 2u && dir[start] == '.' && dir[start + 1u] == '.') {
            return false;
        }
        if (len == 0u) {
            return false; /* empty component: "a//b" or a trailing slash */
        }
        if (dir[i] == '\0') {
            break;
        }
        nested = true;
        start  = i + 1u;
    }
    return nested;
}

enum spg_status spg_fixture_reset(const char *dir) {
    if (!dir_is_deletable(dir)) {
        return SPG_E_INVALID_ARG;
    }
    const char *rm[] = {"rm", "-rf", "--", dir, nullptr};
    if (!run_to_completion(rm)) {
        return SPG_E_IO;
    }
    const char *mk[] = {"mkdir", "-p", "--", dir, nullptr};
    if (!run_to_completion(mk)) {
        return SPG_E_IO;
    }
    return SPG_OK;
}

static bool is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

enum spg_status spg_fixture_copy_into(const char *dst_dir,
                                      const char *src_dir) {
    if (src_dir == nullptr || src_dir[0] == '\0') {
        return SPG_OK; /* no fixture declared: nothing to seed */
    }
    if (dst_dir == nullptr || dst_dir[0] == '\0') {
        return SPG_E_INVALID_ARG;
    }
    if (!is_dir(src_dir)) {
        return SPG_E_NOT_FOUND;
    }
    if (!is_dir(dst_dir)) {
        return SPG_E_INVALID_ARG;
    }
    /* "src/." copies the CONTENTS, so a second call with another source
     * overlays rather than nesting. */
    char src[4096];
    const int n = snprintf(src, sizeof src, "%s/.", src_dir);
    if (n < 0 || (size_t)n >= sizeof src) {
        return SPG_E_LIMIT;
    }
    const char *cp[] = {"cp", "-R", src, dst_dir, nullptr};
    return run_to_completion(cp) ? SPG_OK : SPG_E_IO;
}
