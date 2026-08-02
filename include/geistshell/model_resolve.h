#ifndef GEISTSHELL_MODEL_RESOLVE_H
#define GEISTSHELL_MODEL_RESOLVE_H

#include "geistshell/status.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct spg_model_resolve_opts {
    const char *explicit_path; /* from --model; nullptr/empty = none */
    bool        allow_download;/* false disables the curl fetch */
};

/* Resolve the GGUF model path the application should load. Tries, in order:
 *   1. opts->explicit_path
 *   2. the GEISTSHELL_MODEL environment variable
 *   3. the default ./gguf_artifacts/gemma4-e2b-Q4_K_M.gguf
 *
 * A path from (1) or (2) that does not exist returns SPG_E_NOT_FOUND (a path
 * the user named is never auto-downloaded). The default (3), when missing and
 * opts->allow_download is true, is fetched with curl from the Gemma 4 GGUF
 * (override the URL via GEISTSHELL_MODEL_URL). On success the chosen path is
 * written to out_path and *downloaded reports whether a fetch happened.
 *
 * Returns SPG_E_INVALID_ARG on null arguments, SPG_E_LIMIT if out_path is too
 * small, SPG_E_NOT_FOUND if the model is absent and not downloaded, and
 * SPG_E_IO on a failed (or still-missing) download. downloaded may be null. */
[[nodiscard]] enum spg_status
spg_model_resolve(const struct spg_model_resolve_opts *opts, size_t out_cap,
                  char out_path[], bool *downloaded);

#ifdef __cplusplus
}
#endif

#endif
