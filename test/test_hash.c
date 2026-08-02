#include "geistshell/hash.h"

#include <stdio.h>
#include <string.h>

static int hex_nibble(const char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + c - 'a';
    }
    return -1;
}

static int expect_hex(const uint8_t got[static SPG_HASH_BYTES],
                      const char *hex) {
    for (size_t i = 0u; i < SPG_HASH_BYTES; i += 1u) {
        const int hi = hex_nibble(hex[i * 2u]);
        const int lo = hex_nibble(hex[(i * 2u) + 1u]);
        if (hi < 0 || lo < 0) {
            return 1;
        }
        const uint8_t expected = (uint8_t)(((uint8_t)hi << 4u) | (uint8_t)lo);
        if (got[i] != expected) {
            return 1;
        }
    }
    return 0;
}

static int test_empty_vector(void) {
    uint8_t out[SPG_HASH_BYTES] = {};
    if (spg_hash_bytes(0u, (const uint8_t *)"", sizeof(out), out) != SPG_OK) {
        return 1;
    }
    return expect_hex(
        out,
        "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262");
}

static int test_abc_vector(void) {
    const uint8_t input[] = {'a', 'b', 'c'};
    uint8_t       out[SPG_HASH_BYTES] = {};
    if (spg_hash_bytes(sizeof(input), input, sizeof(out), out) != SPG_OK) {
        return 1;
    }
    return expect_hex(
        out,
        "6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85");
}

static int test_streaming_matches_one_shot(void) {
    uint8_t input[3000];
    for (size_t i = 0u; i < sizeof(input); i += 1u) {
        input[i] = (uint8_t)(i * 31u + 7u);
    }

    uint8_t one_shot[SPG_HASH_BYTES] = {};
    uint8_t streamed[SPG_HASH_BYTES] = {};
    if (spg_hash_bytes(sizeof(input), input, sizeof(one_shot), one_shot) !=
        SPG_OK) {
        return 1;
    }

    struct spg_hash_state state = {};
    spg_hash_init(&state);
    if (spg_hash_update(&state, 17u, input) != SPG_OK) {
        return 1;
    }
    if (spg_hash_update(&state, 1000u, input + 17u) != SPG_OK) {
        return 1;
    }
    if (spg_hash_update(&state, sizeof(input) - 1017u, input + 1017u) !=
        SPG_OK) {
        return 1;
    }
    if (spg_hash_final(&state, sizeof(streamed), streamed) != SPG_OK) {
        return 1;
    }
    return memcmp(one_shot, streamed, sizeof(one_shot)) == 0 ? 0 : 1;
}

static int test_invalid_args(void) {
    uint8_t out[SPG_HASH_BYTES] = {};
    if (spg_hash_update(nullptr, 0u, nullptr) != SPG_E_INVALID_ARG) {
        return 1;
    }
    if (spg_hash_bytes(1u, nullptr, sizeof(out), out) != SPG_E_INVALID_ARG) {
        return 1;
    }
    if (spg_hash_final(nullptr, sizeof(out), out) != SPG_E_INVALID_ARG) {
        return 1;
    }
    return 0;
}

int main(void) {
    if (test_empty_vector() != 0) {
        fprintf(stderr, "test_empty_vector failed\n");
        return 1;
    }
    if (test_abc_vector() != 0) {
        fprintf(stderr, "test_abc_vector failed\n");
        return 1;
    }
    if (test_streaming_matches_one_shot() != 0) {
        fprintf(stderr, "test_streaming_matches_one_shot failed\n");
        return 1;
    }
    if (test_invalid_args() != 0) {
        fprintf(stderr, "test_invalid_args failed\n");
        return 1;
    }
    return 0;
}
