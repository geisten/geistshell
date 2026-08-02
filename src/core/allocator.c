#include "geistshell/allocator.h"

#include <stdint.h>

enum spg_status spg_arena_create(struct spg_arena *arena, const size_t bytes) {
    if (arena == nullptr || bytes == 0u) {
        return SPG_E_INVALID_ARG;
    }
    if (!try_create_memory_arena(&arena->inner, bytes)) {
        return SPG_E_OOM;
    }
    return SPG_OK;
}

void spg_arena_destroy(struct spg_arena *arena) {
    if (arena == nullptr) {
        return;
    }
    free_memory_arena(&arena->inner);
}

void spg_arena_reset(struct spg_arena *arena) {
    if (arena == nullptr) {
        return;
    }
    arena->inner.used = 0u;
}

enum spg_status spg_arena_alloc(struct spg_arena *arena, const size_t bytes,
                                const size_t alignment, void **out) {
    if (out == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out = nullptr;
    if (arena == nullptr || bytes == 0u || alignment == 0u) {
        return SPG_E_INVALID_ARG;
    }
    if ((alignment & (alignment - 1u)) != 0u) {
        return SPG_E_INVALID_ARG;
    }

    *out = arena_allocate_aligned(&arena->inner, bytes, alignment);
    if (*out == nullptr) {
        return SPG_E_OOM;
    }
    return SPG_OK;
}
