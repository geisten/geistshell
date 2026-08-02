#define _POSIX_C_SOURCE 200809L

#include "geistshell/model_resolve.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TMP_MODEL "/tmp/spg_model_resolve_test.gguf"
#define MISSING   "/tmp/spg_model_resolve_missing_xyz.gguf"

static int make_temp(void) {
    FILE *f = fopen(TMP_MODEL, "wb");
    if (f == nullptr) {
        return 1;
    }
    fputs("gguf", f);
    return fclose(f) == 0 ? 0 : 1;
}

static int test_explicit_existing(void) {
    (void)unsetenv("GEISTSHELL_MODEL");
    char out[256] = {0};
    bool downloaded = true;
    const struct spg_model_resolve_opts opts = {.explicit_path = TMP_MODEL,
                                                .allow_download = false};
    if (spg_model_resolve(&opts, sizeof out, out, &downloaded) != SPG_OK) {
        return 1;
    }
    return (strcmp(out, TMP_MODEL) == 0 && !downloaded) ? 0 : 1;
}

static int test_explicit_missing(void) {
    (void)unsetenv("GEISTSHELL_MODEL");
    (void)unlink(MISSING);
    char out[256] = {0};
    const struct spg_model_resolve_opts opts = {.explicit_path = MISSING,
                                                .allow_download = false};
    /* A named-but-missing path is never downloaded. */
    return spg_model_resolve(&opts, sizeof out, out, nullptr) == SPG_E_NOT_FOUND
               ? 0
               : 1;
}

static int test_env_override(void) {
    if (setenv("GEISTSHELL_MODEL", TMP_MODEL, 1) != 0) {
        return 1;
    }
    char out[256] = {0};
    const struct spg_model_resolve_opts opts = {.explicit_path  = nullptr,
                                                .allow_download = false};
    const enum spg_status st =
        spg_model_resolve(&opts, sizeof out, out, nullptr);
    (void)unsetenv("GEISTSHELL_MODEL");
    return (st == SPG_OK && strcmp(out, TMP_MODEL) == 0) ? 0 : 1;
}

static int test_buffer_too_small(void) {
    (void)unsetenv("GEISTSHELL_MODEL");
    char out[4] = {0};
    const struct spg_model_resolve_opts opts = {.explicit_path = TMP_MODEL,
                                                .allow_download = false};
    return spg_model_resolve(&opts, sizeof out, out, nullptr) == SPG_E_LIMIT
               ? 0
               : 1;
}

static int test_invalid_args(void) {
    char out[256] = {0};
    bool dl = false;
    const struct spg_model_resolve_opts opts = {.explicit_path  = TMP_MODEL,
                                                .allow_download = false};
    if (spg_model_resolve(nullptr, sizeof out, out, &dl) != SPG_E_INVALID_ARG) {
        return 1;
    }
    if (spg_model_resolve(&opts, 0u, out, &dl) != SPG_E_INVALID_ARG) {
        return 1;
    }
    if (spg_model_resolve(&opts, sizeof out, nullptr, &dl) !=
        SPG_E_INVALID_ARG) {
        return 1;
    }
    return 0;
}

int main(void) {
    if (make_temp() != 0) {
        fprintf(stderr, "make_temp failed\n");
        return 1;
    }
    int rc = 0;
    if (test_explicit_existing() != 0) {
        fprintf(stderr, "test_explicit_existing failed\n");
        rc = 1;
    }
    if (test_explicit_missing() != 0) {
        fprintf(stderr, "test_explicit_missing failed\n");
        rc = 1;
    }
    if (test_env_override() != 0) {
        fprintf(stderr, "test_env_override failed\n");
        rc = 1;
    }
    if (test_buffer_too_small() != 0) {
        fprintf(stderr, "test_buffer_too_small failed\n");
        rc = 1;
    }
    if (test_invalid_args() != 0) {
        fprintf(stderr, "test_invalid_args failed\n");
        rc = 1;
    }
    (void)unlink(TMP_MODEL);
    return rc;
}
