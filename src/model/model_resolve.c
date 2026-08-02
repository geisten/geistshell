#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#    define _DARWIN_C_SOURCE 1
#endif

#include "geistshell/model_resolve.h"

#include <errno.h>
#include <limits.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PATH_MAX
#    define PATH_MAX 4096
#endif

extern char **environ;

#define SPG_MODEL_DEFAULT_PATH "gguf_artifacts/gemma4-e2b-Q4_K_M.gguf"
#define SPG_MODEL_DEFAULT_URL                                                  \
    "https://huggingface.co/unsloth/gemma-4-E2B-it-GGUF/resolve/main/"         \
    "gemma-4-E2B-it-Q4_K_M.gguf"

static bool file_readable(const char *path) {
    return access(path, R_OK) == 0;
}

static enum spg_status copy_out(char *out, const size_t cap, const char *src) {
    const size_t n = strlen(src);
    if (n + 1u > cap) {
        return SPG_E_LIMIT;
    }
    memcpy(out, src, n + 1u);
    return SPG_OK;
}

/* Best-effort create of the immediate parent directory of path. */
static void ensure_parent_dir(const char *path) {
    const char *slash = strrchr(path, '/');
    if (slash == nullptr || slash == path) {
        return;
    }
    const size_t n = (size_t)(slash - path);
    char         dir[PATH_MAX];
    if (n >= sizeof dir) {
        return;
    }
    memcpy(dir, path, n);
    dir[n] = '\0';
    (void)mkdir(dir, 0755); /* ignore EEXIST and other errors */
}

/* Download url to dest via curl, resumable through a .part sidecar. curl
 * inherits our stdio so its progress meter is visible. */
static enum spg_status fetch_with_curl(const char *url, const char *dest) {
    ensure_parent_dir(dest);

    char part[PATH_MAX];
    const int part_n = snprintf(part, sizeof part, "%s.part", dest);
    if (part_n < 0 || (size_t)part_n >= sizeof part) {
        return SPG_E_LIMIT;
    }

    fprintf(stderr,
            "geistshell: model not found, downloading (~3.1 GB)\n  from %s\n"
            "  to   %s\n",
            url, dest);

    const char *argv[] = {"curl", "-fL",  "--retry", "3",  "--retry-delay",
                          "2",    "-C",   "-",       "-o", part,
                          url,    nullptr};
    pid_t     pid = 0;
    const int rc  = posix_spawnp(&pid, "curl", nullptr, nullptr,
                                 (char *const *)argv, environ);
    if (rc != 0) {
        fprintf(stderr,
                "geistshell: cannot run curl (%s); install curl or run "
                "`make fetch-model`\n",
                strerror(rc));
        return SPG_E_IO;
    }

    int   wstatus = 0;
    pid_t w       = 0;
    do {
        w = waitpid(pid, &wstatus, 0);
    } while (w < 0 && errno == EINTR);
    if (w != pid || !WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) {
        fprintf(stderr,
                "geistshell: download failed; partial file kept for resume "
                "at %s\n",
                part);
        return SPG_E_IO;
    }

    if (rename(part, dest) != 0) {
        return SPG_E_IO;
    }
    return SPG_OK;
}

enum spg_status spg_model_resolve(const struct spg_model_resolve_opts *opts,
                                  const size_t out_cap, char out_path[],
                                  bool *downloaded) {
    if (opts == nullptr || out_path == nullptr || out_cap == 0u) {
        return SPG_E_INVALID_ARG;
    }
    if (downloaded != nullptr) {
        *downloaded = false;
    }
    out_path[0] = '\0';

    const char *named = nullptr;
    if (opts->explicit_path != nullptr && opts->explicit_path[0] != '\0') {
        named = opts->explicit_path;
    } else {
        const char *env = getenv("GEISTSHELL_MODEL");
        if (env != nullptr && env[0] != '\0') {
            named = env;
        }
    }
    if (named != nullptr) {
        if (!file_readable(named)) {
            return SPG_E_NOT_FOUND;
        }
        return copy_out(out_path, out_cap, named);
    }

    if (file_readable(SPG_MODEL_DEFAULT_PATH)) {
        return copy_out(out_path, out_cap, SPG_MODEL_DEFAULT_PATH);
    }
    if (!opts->allow_download) {
        return SPG_E_NOT_FOUND;
    }

    const char *url = getenv("GEISTSHELL_MODEL_URL");
    if (url == nullptr || url[0] == '\0') {
        url = SPG_MODEL_DEFAULT_URL;
    }
    const enum spg_status st = fetch_with_curl(url, SPG_MODEL_DEFAULT_PATH);
    if (st != SPG_OK) {
        return st;
    }
    if (!file_readable(SPG_MODEL_DEFAULT_PATH)) {
        return SPG_E_IO;
    }
    if (downloaded != nullptr) {
        *downloaded = true;
    }
    return copy_out(out_path, out_cap, SPG_MODEL_DEFAULT_PATH);
}
