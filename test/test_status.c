#include "geistshell/allocator.h"
#include "geistshell/status.h"

#include <stdalign.h>
#include <stdint.h>
#include <stdio.h>

static int test_status_strings(void) {
    if (spg_status_to_string(SPG_OK) == nullptr) {
        return 1;
    }
    if (spg_status_to_string((enum spg_status)9999) == nullptr) {
        return 1;
    }
    return 0;
}

static int test_arena_alloc(void) {
    struct spg_arena arena = {};
    void            *ptr   = nullptr;

    if (spg_arena_create(&arena, 1024u) != SPG_OK) {
        return 1;
    }
    if (spg_arena_alloc(&arena, sizeof(uint64_t), alignof(uint64_t), &ptr) !=
        SPG_OK) {
        spg_arena_destroy(&arena);
        return 1;
    }
    if (ptr == nullptr) {
        spg_arena_destroy(&arena);
        return 1;
    }
    spg_arena_destroy(&arena);
    return 0;
}

static int test_arena_negative_args(void) {
    struct spg_arena arena = {};
    void            *ptr   = (void *)1;

    if (spg_arena_create(nullptr, 1024u) != SPG_E_INVALID_ARG) {
        return 1;
    }
    if (spg_arena_create(&arena, 0u) != SPG_E_INVALID_ARG) {
        return 1;
    }
    if (spg_arena_alloc(nullptr, 1u, 1u, &ptr) != SPG_E_INVALID_ARG) {
        return 1;
    }
    if (ptr != nullptr) {
        return 1;
    }
    if (spg_arena_alloc(&arena, 1u, 3u, &ptr) != SPG_E_INVALID_ARG) {
        return 1;
    }
    return 0;
}

int main(void) {
    if (test_status_strings() != 0) {
        fprintf(stderr, "test_status_strings failed\n");
        return 1;
    }
    if (test_arena_alloc() != 0) {
        fprintf(stderr, "test_arena_alloc failed\n");
        return 1;
    }
    if (test_arena_negative_args() != 0) {
        fprintf(stderr, "test_arena_negative_args failed\n");
        return 1;
    }
    return 0;
}
