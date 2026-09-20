#ifndef GEISTSHELL_MEM_STORE_H
#define GEISTSHELL_MEM_STORE_H

#include "geistshell/status.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Persistent, human-readable long-term memory ("mind palace"): one Markdown
 * file per memory in a directory, plus a generated one-line-hook index. This
 * is the durable, cross-run store, distinct from the ephemeral structured
 * spg_memory fact store. Operations write atomically and stay bounded. */

#define SPG_MEM_MAX_FILES  128u
#define SPG_MEM_SLUG_MAX   64u
#define SPG_MEM_DESC_MAX   256u
#define SPG_MEM_BODY_MAX   65536u
#define SPG_MEM_INDEX_TOPK 24u /* index lines injected before the "N more" pointer */

#ifndef SPG_MEM_PATH_MAX
#    define SPG_MEM_PATH_MAX 4096
#endif

/* Upper bound on the rendered index: SPG_MEM_INDEX_TOPK lines (each at most
 * "- <slug>: <desc>\n") plus the trailing "... N more" pointer line. */
#define SPG_MEM_INDEX_CACHE_BYTES                                              \
    (SPG_MEM_INDEX_TOPK * (SPG_MEM_SLUG_MAX + SPG_MEM_DESC_MAX + 8u) + 64u)

struct spg_mem_store {
    char dir[SPG_MEM_PATH_MAX];
    /* Lazily rebuilt cache of the rendered mind-palace index, invalidated on
     * save/delete. Lets spg_mem_index serve the per-tick context-injection path
     * without re-scanning (and re-parsing) the whole directory each call. The
     * cache is only consistent for mutations made through this same store
     * instance, which matches the single-threaded, one-store-per-run design. */
    char   index_cache[SPG_MEM_INDEX_CACHE_BYTES];
    size_t index_len;       /* rendered bytes held in index_cache */
    bool   index_truncated; /* cached *truncated out-value */
    bool   index_valid;     /* false => cache cold/stale, rebuild on next read */
};

/* Bind store to a directory, creating it if missing. Returns SPG_E_INVALID_ARG
 * on null/empty, SPG_E_LIMIT if the path is too long, SPG_E_IO if the directory
 * cannot be created. */
[[nodiscard]] enum spg_status spg_mem_store_open(struct spg_mem_store *store,
                                                 const char           *dir);

/* Resolve a memory directory: an explicit flag (when non-null/non-empty) wins,
 * else $GEISTSHELL_MEMORY_DIR, else the "memory" default. The shared policy for
 * CLI/chat surfaces that default a store; never returns null. */
[[nodiscard]] const char *spg_mem_resolve_dir(const char *flag);

/* True when slug is a safe identifier: 1..SPG_MEM_SLUG_MAX bytes of [a-z0-9-]
 * only (no path separators, dots, uppercase). All file paths are built from a
 * validated slug, never from raw model input. */
[[nodiscard]] bool spg_mem_slug_valid(const char *slug);

/* True when slug sits in a namespace the learning loop owns: "lesson-",
 * "skill-" or "pref-".
 *
 * These are not ordinary memories. agent_loop reads lesson-* back as a
 * `(directive ...)` and puts it in front of the model, and the improve loop
 * decides which lessons survive by measuring them. A model that could write
 * its own lesson would hand itself an instruction and bypass that gate, and one
 * that could delete a lesson or a pref would edit the learning state just as
 * effectively. So the public save/delete refuse this namespace, and only the
 * loop's own writers (spg_mem_save_reserved, spg_mem_delete_reserved) may touch
 * it. Reading stays open: a lesson reaches the model as context anyway. */
[[nodiscard]] bool spg_mem_slug_reserved(const char *slug);

/* Upsert a memory: write <dir>/<slug>.md with frontmatter (name, description)
 * and the Markdown body, then regenerate the index. Overwrites an existing slug
 * atomically. Returns SPG_E_INVALID_ARG (bad slug/null/description with a
 * newline) or SPG_E_LIMIT (description/body over cap, or a new slug beyond
 * SPG_MEM_MAX_FILES), or SPG_E_POLICY_DENIED for a reserved slug.
 *
 * This is the path every model-driven surface takes (the memory executor and
 * the chat tool), so the refusal lives here rather than in those callers: a
 * surface added later is denied by default instead of having to remember. */
[[nodiscard]] enum spg_status spg_mem_save(struct spg_mem_store *store,
                                           const char           *slug,
                                           const char           *description,
                                           const char           *body);

/* Delete a memory and regenerate the index. SPG_E_NOT_FOUND if absent,
 * SPG_E_POLICY_DENIED for a reserved slug. */
[[nodiscard]] enum spg_status spg_mem_delete(struct spg_mem_store *store,
                                             const char           *slug);

/* Save and delete for the learning loop and the operator CLI: identical to the
 * two above but allowed in the reserved namespaces. Callers are the improve and
 * distill paths, pref.c, and `geistshell memory save/delete` — the operator is
 * trusted by SECURITY.md's threat model, the model is not. Never reachable from
 * a model recommendation. */
[[nodiscard]] enum spg_status spg_mem_save_reserved(struct spg_mem_store *store,
                                                    const char *slug,
                                                    const char *description,
                                                    const char *body);

[[nodiscard]] enum spg_status
spg_mem_delete_reserved(struct spg_mem_store *store, const char *slug);

/* Read the full file content of a memory into dst. SPG_E_NOT_FOUND if absent,
 * SPG_E_LIMIT if dst is too small (dst gets the truncated, NUL-terminated
 * prefix); *out_required (may be null) receives the full byte length. */
[[nodiscard]] enum spg_status spg_mem_read(struct spg_mem_store *store,
                                           const char *slug, size_t dst_cap,
                                           char dst[], size_t *out_required);

/* Render the index for context injection: "- <slug>: <description>\n" per
 * memory (slug-sorted), capped at SPG_MEM_INDEX_TOPK lines followed by a
 * "- ... N more (memory list)\n" pointer. *truncated (may be null) is set when
 * the pointer is emitted; *out_required (may be null) receives the full length.
 * SPG_E_LIMIT if dst is too small. */
[[nodiscard]] enum spg_status spg_mem_index(struct spg_mem_store *store,
                                            size_t dst_cap, char dst[],
                                            size_t *out_required,
                                            bool   *truncated);

/* The one-line DIRECTIVE (description) of a single memory, for slug-triggered
 * auto-injection on small models (docs/LEARNING.md P6): when the loop hits a
 * failure whose slug names a stored lesson, its directive is injected without
 * the model having to recall it, and the full body is left in the mind-palace
 * for larger models. budget_bytes caps the injected length (0 = the store's
 * description cap); an over-budget or missing directive writes nothing.
 * Writes a NUL-terminated directive into dst[0..dst_cap) and returns its
 * length, or 0 when the slug has no memory / does not fit / on bad args. */
size_t spg_mem_directive(struct spg_mem_store *store, const char *slug,
                         size_t budget_bytes, size_t dst_cap, char dst[]);

/* Enumerate memory slugs (slug-sorted) into slugs[0..*count). Returns
 * SPG_E_LIMIT if there are more than cap (the first cap are written). */
[[nodiscard]] enum spg_status
spg_mem_list(struct spg_mem_store *store, size_t cap,
             char slugs[][SPG_MEM_SLUG_MAX + 1u], size_t *count);

#ifdef __cplusplus
}
#endif

#endif
