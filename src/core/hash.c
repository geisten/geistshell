#include "geistshell/hash.h"

#include <stdbool.h>
#include <string.h>

enum blake3_flags {
    CHUNK_START = 1 << 0,
    CHUNK_END   = 1 << 1,
    PARENT      = 1 << 2,
    ROOT        = 1 << 3,
};

struct blake3_output {
    uint32_t input_cv[8];
    uint32_t block_words[16];
    uint64_t counter;
    uint32_t block_len;
    uint32_t flags;
};

static const uint32_t iv[8] = {
    0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
    0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u,
};

static const uint8_t msg_permutation[16] = {
    2u, 6u, 3u, 10u, 7u, 0u, 4u, 13u,
    1u, 11u, 12u, 5u, 9u, 14u, 15u, 8u,
};

static uint32_t rotr32(const uint32_t x, const uint32_t n) {
    return (x >> n) | (x << (32u - n));
}

static void store32(uint8_t out[static 4], const uint32_t word) {
    out[0] = (uint8_t)(word & 0xffu);
    out[1] = (uint8_t)((word >> 8u) & 0xffu);
    out[2] = (uint8_t)((word >> 16u) & 0xffu);
    out[3] = (uint8_t)((word >> 24u) & 0xffu);
}

/* Load up to 64 bytes of a block as 16 little-endian 32-bit words. The BLAKE3
 * block is defined little-endian, so on a little-endian host the bytes map
 * directly: copy the n present bytes and zero-pad the rest (one memcpy + one
 * memset instead of a per-byte shift/or loop). Big-endian hosts fall back to the
 * explicit packing. */
static void words_from_block(size_t n, const uint8_t block[],
                             uint32_t words[static 16]) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    memcpy(words, block, n);
    memset((uint8_t *)words + n, 0, 16u * sizeof(uint32_t) - n);
#else
    memset(words, 0, 16u * sizeof(uint32_t));
    for (size_t i = 0u; i < n; i += 1u) {
        words[i / 4u] |= (uint32_t)block[i] << (8u * (uint32_t)(i % 4u));
    }
#endif
}

static void g(uint32_t state[static 16], const size_t a, const size_t b,
              const size_t c, const size_t d, const uint32_t mx,
              const uint32_t my) {
    state[a] = state[a] + state[b] + mx;
    state[d] = rotr32(state[d] ^ state[a], 16u);
    state[c] = state[c] + state[d];
    state[b] = rotr32(state[b] ^ state[c], 12u);
    state[a] = state[a] + state[b] + my;
    state[d] = rotr32(state[d] ^ state[a], 8u);
    state[c] = state[c] + state[d];
    state[b] = rotr32(state[b] ^ state[c], 7u);
}

static void round_fn(uint32_t state[static 16],
                     const uint32_t msg[static 16]) {
    g(state, 0u, 4u, 8u, 12u, msg[0], msg[1]);
    g(state, 1u, 5u, 9u, 13u, msg[2], msg[3]);
    g(state, 2u, 6u, 10u, 14u, msg[4], msg[5]);
    g(state, 3u, 7u, 11u, 15u, msg[6], msg[7]);
    g(state, 0u, 5u, 10u, 15u, msg[8], msg[9]);
    g(state, 1u, 6u, 11u, 12u, msg[10], msg[11]);
    g(state, 2u, 7u, 8u, 13u, msg[12], msg[13]);
    g(state, 3u, 4u, 9u, 14u, msg[14], msg[15]);
}

static void permute(uint32_t msg[static 16]) {
    uint32_t permuted[16] = {};
    for (size_t i = 0u; i < 16u; i += 1u) {
        permuted[i] = msg[msg_permutation[i]];
    }
    memcpy(msg, permuted, sizeof(permuted));
}

static void compress(const uint32_t cv[static 8],
                     const uint32_t block_words[static 16],
                     const uint64_t counter, const uint32_t block_len,
                     const uint32_t flags, uint32_t out[static 16]) {
    uint32_t state[16] = {
        cv[0], cv[1], cv[2], cv[3], cv[4], cv[5], cv[6], cv[7],
        iv[0], iv[1], iv[2], iv[3],
        (uint32_t)counter, (uint32_t)(counter >> 32u), block_len, flags,
    };
    uint32_t msg[16] = {};
    memcpy(msg, block_words, sizeof(msg));

    for (size_t round = 0u; round < 7u; round += 1u) {
        round_fn(state, msg);
        if (round != 6u) {
            permute(msg);
        }
    }

    for (size_t i = 0u; i < 8u; i += 1u) {
        out[i]     = state[i] ^ state[i + 8u];
        out[i + 8] = state[i + 8u] ^ cv[i];
    }
}

static void output_chaining_value(const struct blake3_output *output,
                                  uint32_t cv[static 8]) {
    uint32_t words[16] = {};
    compress(output->input_cv, output->block_words, output->counter,
             output->block_len, output->flags, words);
    memcpy(cv, words, 8u * sizeof(uint32_t));
}

static void output_root_bytes(const struct blake3_output *output, size_t out_n,
                              uint8_t out[static out_n]) {
    uint64_t output_block_counter = 0u;
    size_t   produced             = 0u;
    while (produced < out_n) {
        uint32_t words[16] = {};
        uint8_t  block[64] = {};
        compress(output->input_cv, output->block_words, output_block_counter,
                 output->block_len, output->flags | ROOT, words);
        for (size_t i = 0u; i < 16u; i += 1u) {
            store32(block + (i * 4u), words[i]);
        }
        const size_t take = out_n - produced < 64u ? out_n - produced : 64u;
        memcpy(out + produced, block, take);
        produced += take;
        output_block_counter += 1u;
    }
}

static struct blake3_output chunk_output(const uint8_t chunk[static 1],
                                         const size_t chunk_len,
                                         const uint64_t chunk_counter) {
    uint32_t cv[8] = {};
    memcpy(cv, iv, sizeof(cv));

    struct blake3_output output = {
        .counter   = chunk_counter,
        .block_len = 0u,
        .flags     = CHUNK_START | CHUNK_END,
    };

    size_t offset = 0u;
    while (offset < chunk_len || (chunk_len == 0u && offset == 0u)) {
        const size_t remaining = chunk_len - offset;
        const size_t block_len =
            remaining < SPG_HASH_BLOCK_BYTES ? remaining : SPG_HASH_BLOCK_BYTES;

        const bool is_start = offset == 0u;
        const bool is_end   = offset + block_len == chunk_len;

        /* Fill the output's fields directly: load the block words in place
         * (words_from_block writes all 64 bytes) instead of staging them in a
         * local and copying. The running chaining value feeds input_cv. */
        memcpy(output.input_cv, cv, sizeof(cv));
        words_from_block(block_len, chunk + offset, output.block_words);
        output.counter   = chunk_counter;
        output.block_len  = (uint32_t)block_len;
        output.flags = (is_start ? CHUNK_START : 0u) | (is_end ? CHUNK_END : 0u);

        if (!is_end) {
            output_chaining_value(&output, cv);
        }

        if (chunk_len == 0u) {
            break;
        }
        offset += block_len;
    }
    return output;
}

static struct blake3_output parent_output(const uint32_t left[static 8],
                                          const uint32_t right[static 8]) {
    struct blake3_output output = {
        .counter   = 0u,
        .block_len = 64u,
        .flags     = PARENT,
    };
    memcpy(output.input_cv, iv, sizeof(output.input_cv));
    memcpy(output.block_words, left, 8u * sizeof(uint32_t));
    memcpy(output.block_words + 8u, right, 8u * sizeof(uint32_t));
    return output;
}

static void parent_cv(const uint32_t left[static 8],
                      const uint32_t right[static 8], uint32_t out[static 8]) {
    const struct blake3_output output = parent_output(left, right);
    output_chaining_value(&output, out);
}

static enum spg_status push_cv(struct spg_hash_state *state,
                               const uint32_t cv_in[static 8],
                               uint64_t total_chunks) {
    uint32_t cv[8] = {};
    memcpy(cv, cv_in, sizeof(cv));
    while ((total_chunks & 1u) == 0u) {
        if (state->cv_stack_len == 0u) {
            return SPG_E_INTERNAL;
        }
        uint32_t left[8] = {};
        state->cv_stack_len -= 1u;
        memcpy(left, state->cv_stack[state->cv_stack_len], sizeof(left));
        parent_cv(left, cv, cv);
        total_chunks >>= 1u;
    }
    if (state->cv_stack_len >= SPG_HASH_MAX_DEPTH) {
        return SPG_E_LIMIT;
    }
    memcpy(state->cv_stack[state->cv_stack_len], cv, sizeof(cv));
    state->cv_stack_len += 1u;
    return SPG_OK;
}

static enum spg_status flush_chunk(struct spg_hash_state *state) {
    if (state->chunk_len == 0u) {
        return SPG_OK;
    }
    const struct blake3_output output =
        chunk_output(state->chunk, state->chunk_len, state->chunk_counter);
    uint32_t cv[8] = {};
    output_chaining_value(&output, cv);
    state->chunk_counter += 1u;
    state->chunk_len = 0u;
    return push_cv(state, cv, state->chunk_counter);
}

void spg_hash_init(struct spg_hash_state *state) {
    if (state == nullptr) {
        return;
    }
    *state = (struct spg_hash_state){};
}

enum spg_status spg_hash_update(struct spg_hash_state *state, const size_t n,
                                const uint8_t data[]) {
    if (state == nullptr || (n > 0u && data == nullptr)) {
        return SPG_E_INVALID_ARG;
    }

    size_t offset = 0u;
    while (offset < n) {
        const size_t space = SPG_HASH_CHUNK_BYTES - state->chunk_len;
        const size_t take  = n - offset < space ? n - offset : space;
        memcpy(state->chunk + state->chunk_len, data + offset, take);
        state->chunk_len += take;
        offset += take;

        if (state->chunk_len == SPG_HASH_CHUNK_BYTES && offset < n) {
            const enum spg_status status = flush_chunk(state);
            if (status != SPG_OK) {
                return status;
            }
        }
    }
    return SPG_OK;
}

enum spg_status spg_hash_final(struct spg_hash_state *state, const size_t out_n,
                               uint8_t out[static out_n]) {
    if (state == nullptr || out == nullptr || out_n == 0u) {
        return SPG_E_INVALID_ARG;
    }

    struct blake3_output output = chunk_output(
        state->chunk, state->chunk_len, state->chunk_counter);
    uint32_t right[8] = {};
    output_chaining_value(&output, right);

    while (state->cv_stack_len > 0u) {
        uint32_t left[8] = {};
        state->cv_stack_len -= 1u;
        memcpy(left, state->cv_stack[state->cv_stack_len], sizeof(left));
        output = parent_output(left, right);
        output_chaining_value(&output, right);
    }

    output_root_bytes(&output, out_n, out);
    return SPG_OK;
}

enum spg_status spg_hash_bytes(const size_t input_n,
                               const uint8_t input[],
                               const size_t out_n, uint8_t out[static out_n]) {
    if ((input_n > 0u && input == nullptr) || out == nullptr || out_n == 0u) {
        return SPG_E_INVALID_ARG;
    }
    struct spg_hash_state state = {};
    spg_hash_init(&state);
    enum spg_status status = spg_hash_update(&state, input_n, input);
    if (status != SPG_OK) {
        return status;
    }
    return spg_hash_final(&state, out_n, out);
}
