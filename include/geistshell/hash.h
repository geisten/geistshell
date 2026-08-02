#ifndef GEISTSHELL_HASH_H
#define GEISTSHELL_HASH_H

#include "geistshell/status.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPG_HASH_BYTES 32u
#define SPG_HASH_BLOCK_BYTES 64u
#define SPG_HASH_CHUNK_BYTES 1024u
#define SPG_HASH_MAX_DEPTH 54u

struct spg_hash_state {
    uint32_t cv_stack[SPG_HASH_MAX_DEPTH][8];
    size_t   cv_stack_len;

    uint8_t  chunk[SPG_HASH_CHUNK_BYTES];
    size_t   chunk_len;
    uint64_t chunk_counter;
};

void spg_hash_init(struct spg_hash_state *state);
[[nodiscard]] enum spg_status spg_hash_update(struct spg_hash_state *state,
                                              size_t n,
                                              const uint8_t data[]);
[[nodiscard]] enum spg_status spg_hash_final(struct spg_hash_state *state,
                                             size_t out_n,
                                             uint8_t out[static out_n]);
[[nodiscard]] enum spg_status spg_hash_bytes(size_t input_n,
                                             const uint8_t input[],
                                             size_t out_n,
                                             uint8_t out[static out_n]);

#ifdef __cplusplus
}
#endif

#endif
