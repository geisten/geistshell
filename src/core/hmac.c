#include "geistshell/hmac.h"

#include <string.h>

#define HMAC_BLOCK SPG_HASH_BLOCK_BYTES /* 64 */

enum spg_status spg_hmac(const size_t key_n, const uint8_t key[],
                         const size_t msg_n, const uint8_t msg[],
                         const size_t out_n, uint8_t out[static out_n]) {
    if ((key_n > 0u && key == nullptr) || (msg_n > 0u && msg == nullptr) ||
        out == nullptr || out_n != SPG_HASH_BYTES) {
        return SPG_E_INVALID_ARG;
    }

    /* K' = key shortened (by hashing) if longer than the block, then zero-padded
     * to the block size. */
    uint8_t k0[HMAC_BLOCK] = {0};
    if (key_n > HMAC_BLOCK) {
        const enum spg_status s = spg_hash_bytes(key_n, key, SPG_HASH_BYTES, k0);
        if (s != SPG_OK) {
            return s;
        }
    } else if (key_n > 0u) {
        memcpy(k0, key, key_n);
    }

    uint8_t ipad[HMAC_BLOCK];
    uint8_t opad[HMAC_BLOCK];
    for (size_t i = 0u; i < HMAC_BLOCK; i += 1u) {
        ipad[i] = (uint8_t)(k0[i] ^ 0x36u);
        opad[i] = (uint8_t)(k0[i] ^ 0x5cu);
    }

    /* inner = H((K' XOR ipad) || msg) */
    uint8_t               inner[SPG_HASH_BYTES] = {0};
    struct spg_hash_state state                = {};
    spg_hash_init(&state);
    enum spg_status s = spg_hash_update(&state, HMAC_BLOCK, ipad);
    if (s == SPG_OK) {
        s = spg_hash_update(&state, msg_n, msg);
    }
    if (s == SPG_OK) {
        s = spg_hash_final(&state, SPG_HASH_BYTES, inner);
    }
    if (s != SPG_OK) {
        return s;
    }

    /* out = H((K' XOR opad) || inner) */
    spg_hash_init(&state);
    s = spg_hash_update(&state, HMAC_BLOCK, opad);
    if (s == SPG_OK) {
        s = spg_hash_update(&state, SPG_HASH_BYTES, inner);
    }
    if (s == SPG_OK) {
        s = spg_hash_final(&state, SPG_HASH_BYTES, out);
    }
    return s;
}

bool spg_hmac_equal(const uint8_t a[static SPG_HASH_BYTES],
                    const uint8_t b[static SPG_HASH_BYTES]) {
    uint8_t diff = 0u;
    for (size_t i = 0u; i < SPG_HASH_BYTES; i += 1u) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0u;
}
