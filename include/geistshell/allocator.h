#ifndef GEISTSHELL_ALLOCATOR_H
#define GEISTSHELL_ALLOCATOR_H

#include "heap.h" /* allocation policy owned by the geist engine */
#include "geistshell/status.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct spg_arena {
    struct memory_arena inner;
};

[[nodiscard]] enum spg_status spg_arena_create(struct spg_arena *arena,
                                               size_t            bytes);
void                         spg_arena_destroy(struct spg_arena *arena);
void                         spg_arena_reset(struct spg_arena *arena);

[[nodiscard]] enum spg_status spg_arena_alloc(struct spg_arena *arena,
                                              size_t            bytes,
                                              size_t            alignment,
                                              void            **out);

#ifdef __cplusplus
}
#endif

#endif
