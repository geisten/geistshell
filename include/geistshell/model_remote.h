#ifndef GEISTSHELL_MODEL_REMOTE_H
#define GEISTSHELL_MODEL_REMOTE_H

#include "geistshell/model_adapter.h"
#include "geistshell/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* libcurl-backed transport for SPG_MODEL_ADAPTER_REMOTE. These are only built
 * (and only called from model_adapter.c) when compiled with SPG_ENABLE_REMOTE.
 * The implementation uses file-scope scratch buffers and a process-global
 * libcurl init, so it assumes the single-threaded, single-adapter runtime the
 * rest of geistshell already relies on. */

[[nodiscard]] enum spg_status
spg_remote_init(struct spg_model_adapter *adapter,
                const struct spg_model_adapter_config *config);

[[nodiscard]] enum spg_status
spg_remote_generate(struct spg_model_adapter *adapter,
                    const struct spg_model_generate_request *request,
                    struct spg_model_generate_result *result);

void spg_remote_destroy(struct spg_model_adapter *adapter);

#ifdef __cplusplus
}
#endif

#endif
