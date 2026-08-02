#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#    define _DARWIN_C_SOURCE 1
#endif

#include "geistshell/mem_store.h"

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SLUGBUF (SPG_MEM_SLUG_MAX + 1u)
#define LINEBUF 1024u

const char *spg_mem_resolve_dir(const char *flag) {
    if (flag != nullptr && flag[0] != '\0') {
        return flag;
    }
    const char *env = getenv("GEISTSHELL_MEMORY_DIR");
    if (env != nullptr && env[0] != '\0') {
        return env;
    }
    return "memory";
}

bool spg_mem_slug_valid(const char *slug) {
    if (slug == nullptr) {
        return false;
    }
    size_t n = 0u;
    for (; slug[n] != '\0'; n += 1u) {
        const char c = slug[n];
        const bool ok =
            (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) {
            return false;
        }
    }
    return n >= 1u && n <= SPG_MEM_SLUG_MAX;
}

enum spg_status spg_mem_store_open(struct spg_mem_store *store,
                                   const char           *dir) {
    if (store == nullptr || dir == nullptr || dir[0] == '\0') {
        return SPG_E_INVALID_ARG;
    }
    const size_t n = strlen(dir);
    if (n + 1u > sizeof store->dir) {
        return SPG_E_LIMIT;
    }
    memcpy(store->dir, dir, n + 1u);
    store->index_len       = 0u;
    store->index_truncated = false;
    store->index_valid     = false;
    if (mkdir(store->dir, 0755) != 0 && errno != EEXIST) {
        return SPG_E_IO;
    }
    return SPG_OK;
}

static enum spg_status build_path(const struct spg_mem_store *store,
                                  const char *slug, const char *suffix,
                                  char *out, const size_t cap) {
    const int n = snprintf(out, cap, "%s/%s%s", store->dir, slug, suffix);
    if (n < 0 || (size_t)n >= cap) {
        return SPG_E_LIMIT;
    }
    return SPG_OK;
}

static bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* A memory file is "<slug>.md" but not the generated "MEMORY.md" index. */
static bool is_memory_file(const char *name) {
    const size_t n = strlen(name);
    if (n < 4u || strcmp(name + n - 3u, ".md") != 0) {
        return false;
    }
    return strcmp(name, "MEMORY.md") != 0;
}

/* Read "description:" and "updated:" from a file's frontmatter. out is always
 * terminated; *updated defaults to 0 when absent. Either out or updated may be
 * null. */
static void read_meta(const char *path, char *out, const size_t cap,
                      uint64_t *updated) {
    if (out != nullptr) {
        out[0] = '\0';
    }
    if (updated != nullptr) {
        *updated = 0u;
    }
    FILE *f = fopen(path, "rb");
    if (f == nullptr) {
        return;
    }
    char line[LINEBUF];
    bool in_fm = false;
    while (fgets(line, sizeof line, f) != nullptr) {
        const size_t ln = strlen(line);
        if (ln > 0u && line[ln - 1u] == '\n') {
            line[ln - 1u] = '\0';
        }
        if (strcmp(line, "---") == 0) {
            if (!in_fm) {
                in_fm = true;
                continue;
            }
            break;
        }
        if (!in_fm) {
            continue;
        }
        if (out != nullptr && strncmp(line, "description:", 12u) == 0) {
            const char *v = line + 12u;
            while (*v == ' ') {
                v += 1u;
            }
            const size_t vn   = strlen(v);
            const size_t take = vn + 1u > cap ? cap - 1u : vn;
            memcpy(out, v, take);
            out[take] = '\0';
        } else if (updated != nullptr && strncmp(line, "updated:", 8u) == 0) {
            const char *v = line + 8u;
            while (*v == ' ') {
                v += 1u;
            }
            uint64_t n = 0u;
            for (size_t i = 0u; v[i] >= '0' && v[i] <= '9'; i += 1u) {
                n = n * 10u + (uint64_t)(v[i] - '0');
            }
            *updated = n;
        }
    }
    (void)fclose(f);
}

/* Collect memory slugs (without ".md"), slug-sorted, into slugs[0..return). */
static size_t collect_sorted(const struct spg_mem_store *store,
                             char slugs[][SLUGBUF], const size_t max) {
    DIR *d = opendir(store->dir);
    if (d == nullptr) {
        return 0u;
    }
    size_t         count = 0u;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr && count < max) {
        if (!is_memory_file(e->d_name)) {
            continue;
        }
        const size_t nm = strlen(e->d_name) - 3u; /* drop ".md" */
        if (nm + 1u > SLUGBUF) {
            continue;
        }
        char slug[SLUGBUF];
        memcpy(slug, e->d_name, nm);
        slug[nm] = '\0';
        /* insertion sort into slugs[] */
        size_t pos = count;
        while (pos > 0u && strcmp(slugs[pos - 1u], slug) > 0) {
            memcpy(slugs[pos], slugs[pos - 1u], SLUGBUF);
            pos -= 1u;
        }
        memcpy(slugs[pos], slug, nm + 1u);
        count += 1u;
    }
    (void)closedir(d);
    return count;
}

struct mem_entry {
    char     slug[SLUGBUF];
    char     desc[SPG_MEM_DESC_MAX + 1u]; /* frontmatter "description:" */
    uint64_t updated;
};

/* Shared scratch for the directory-collection helpers (save, delete's index
 * regeneration, and the index-cache rebuild). One SPG_MEM_MAX_FILES array (~one
 * mem_entry per file) instead of a separate static per function keeps the BSS
 * footprint to a single buffer. Safe because the mind-palace runs on one thread
 * (the governed loop is single-threaded by design) and these helpers never nest:
 * each fills the scratch and fully drains it before returning, and none calls
 * another that reuses it. NOT reentrant — do not use from a signal handler or a
 * second thread. */
static struct mem_entry g_collect_scratch[SPG_MEM_MAX_FILES];

/* Recency order: higher "updated" first, slug ascending as a stable tiebreak. */
static bool entry_before(const struct mem_entry *a, const struct mem_entry *b) {
    if (a->updated != b->updated) {
        return a->updated > b->updated;
    }
    return strcmp(a->slug, b->slug) < 0;
}

/* Collect memories sorted most-recently-updated first. */
static size_t collect_by_recency(const struct spg_mem_store *store,
                                 struct mem_entry entries[], const size_t max) {
    DIR *d = opendir(store->dir);
    if (d == nullptr) {
        return 0u;
    }
    size_t         count = 0u;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr && count < max) {
        if (!is_memory_file(e->d_name)) {
            continue;
        }
        const size_t nm = strlen(e->d_name) - 3u;
        if (nm + 1u > SLUGBUF) {
            continue;
        }
        struct mem_entry entry = {};
        memcpy(entry.slug, e->d_name, nm);
        entry.slug[nm] = '\0';
        char path[SPG_MEM_PATH_MAX];
        if (build_path(store, entry.slug, ".md", path, sizeof path) != SPG_OK) {
            continue;
        }
        read_meta(path, entry.desc, sizeof entry.desc, &entry.updated);
        size_t pos = count;
        while (pos > 0u && entry_before(&entry, &entries[pos - 1u])) {
            entries[pos] = entries[pos - 1u];
            pos -= 1u;
        }
        entries[pos] = entry;
        count += 1u;
    }
    (void)closedir(d);
    return count;
}

/* Rewrite <dir>/MEMORY.md atomically from `count` recency-ordered entries.
 * Best-effort. When upsert_slug is non-null its line is written first (it models
 * a just-completed save, which always ranks most-recent) using upsert_desc, and
 * any existing entry with that slug is skipped — so a save needs no second
 * directory scan. upsert_desc is leading-space-stripped to stay byte-identical
 * with a read_meta round-trip of the written frontmatter. */
static void write_index(const struct spg_mem_store *store,
                        const struct mem_entry entries[], const size_t count,
                        const char *upsert_slug, const char *upsert_desc) {
    char tmp[SPG_MEM_PATH_MAX];
    char dst[SPG_MEM_PATH_MAX];
    if (build_path(store, "MEMORY", ".md.tmp", tmp, sizeof tmp) != SPG_OK ||
        build_path(store, "MEMORY", ".md", dst, sizeof dst) != SPG_OK) {
        return;
    }
    FILE *f = fopen(tmp, "wb");
    if (f == nullptr) {
        return;
    }
    if (upsert_slug != nullptr) {
        const char *d = upsert_desc;
        while (*d == ' ') {
            d += 1u;
        }
        (void)fprintf(f, "- %s: %s\n", upsert_slug, d);
    }
    for (size_t i = 0u; i < count; i += 1u) {
        if (upsert_slug != nullptr &&
            strcmp(entries[i].slug, upsert_slug) == 0) {
            continue;
        }
        (void)fprintf(f, "- %s: %s\n", entries[i].slug, entries[i].desc);
    }
    if (fclose(f) == 0) {
        (void)rename(tmp, dst);
    } else {
        (void)remove(tmp);
    }
}

/* Rewrite <dir>/MEMORY.md with the full index from a fresh scan. Best-effort. */
static void regenerate_index(const struct spg_mem_store *store) {
    const size_t count =
        collect_by_recency(store, g_collect_scratch, SPG_MEM_MAX_FILES);
    write_index(store, g_collect_scratch, count, nullptr, nullptr);
}

enum spg_status spg_mem_save(struct spg_mem_store *store, const char *slug,
                             const char *description, const char *body) {
    if (store == nullptr || !spg_mem_slug_valid(slug) ||
        description == nullptr || body == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    if (strchr(description, '\n') != nullptr) {
        return SPG_E_INVALID_ARG; /* would break the frontmatter */
    }
    const size_t body_n = strlen(body);
    if (strlen(description) > SPG_MEM_DESC_MAX || body_n > SPG_MEM_BODY_MAX) {
        return SPG_E_LIMIT;
    }

    char path[SPG_MEM_PATH_MAX];
    char tmp[SPG_MEM_PATH_MAX];
    if (build_path(store, slug, ".md", path, sizeof path) != SPG_OK ||
        build_path(store, slug, ".md.tmp", tmp, sizeof tmp) != SPG_OK) {
        return SPG_E_LIMIT;
    }
    /* One pre-write scan supplies everything the save needs: the current entry
     * set (reused to rebuild the index without a second scan), the file count
     * (capacity check) and the highest "updated" counter (recency bump). The
     * scan is recency-sorted, so entries[0] holds the maximum. */
    const size_t count =
        collect_by_recency(store, g_collect_scratch, SPG_MEM_MAX_FILES);
    if (!file_exists(path) && count >= SPG_MEM_MAX_FILES) {
        return SPG_E_LIMIT;
    }

    /* A re-save bumps the slug above every existing memory, so the index ranks
     * it most-recent. */
    const uint64_t updated =
        (count > 0u ? g_collect_scratch[0].updated : 0u) + 1u;

    FILE *f = fopen(tmp, "wb");
    if (f == nullptr) {
        return SPG_E_IO;
    }
    const int w =
        fprintf(f, "---\nname: %s\ndescription: %s\nupdated: %llu\n---\n%s", slug,
                description, (unsigned long long)updated, body);
    const bool body_nl = body_n == 0u || body[body_n - 1u] == '\n';
    if (w >= 0 && !body_nl) {
        (void)fputc('\n', f);
    }
    if (w < 0 || fclose(f) != 0) {
        (void)remove(tmp);
        return SPG_E_IO;
    }
    if (rename(tmp, path) != 0) {
        (void)remove(tmp);
        return SPG_E_IO;
    }
    /* Rebuild the index from the pre-write set plus this upsert — no rescan.
     * g_collect_scratch still holds the pre-write set from above. */
    write_index(store, g_collect_scratch, count, slug, description);
    store->index_valid = false; /* in-RAM read cache is now stale */
    return SPG_OK;
}

enum spg_status spg_mem_delete(struct spg_mem_store *store, const char *slug) {
    if (store == nullptr || !spg_mem_slug_valid(slug)) {
        return SPG_E_INVALID_ARG;
    }
    char path[SPG_MEM_PATH_MAX];
    if (build_path(store, slug, ".md", path, sizeof path) != SPG_OK) {
        return SPG_E_LIMIT;
    }
    if (!file_exists(path)) {
        return SPG_E_NOT_FOUND;
    }
    if (remove(path) != 0) {
        return SPG_E_IO;
    }
    regenerate_index(store);
    store->index_valid = false; /* in-RAM read cache is now stale */
    return SPG_OK;
}

enum spg_status spg_mem_read(struct spg_mem_store *store, const char *slug,
                             const size_t dst_cap, char dst[],
                             size_t *out_required) {
    if (store == nullptr || !spg_mem_slug_valid(slug) || dst == nullptr ||
        dst_cap == 0u) {
        return SPG_E_INVALID_ARG;
    }
    char path[SPG_MEM_PATH_MAX];
    if (build_path(store, slug, ".md", path, sizeof path) != SPG_OK) {
        return SPG_E_LIMIT;
    }
    FILE *f = fopen(path, "rb");
    if (f == nullptr) {
        return SPG_E_NOT_FOUND;
    }
    size_t total = 0u;
    size_t used  = 0u;
    bool   over  = false;
    char   chunk[4096];
    size_t r;
    while ((r = fread(chunk, 1u, sizeof chunk, f)) > 0u) {
        total += r;
        const size_t room = used < dst_cap - 1u ? dst_cap - 1u - used : 0u;
        const size_t take = r < room ? r : room;
        if (take > 0u) {
            memcpy(dst + used, chunk, take);
            used += take;
        }
        if (take < r) {
            over = true;
        }
    }
    dst[used] = '\0';
    (void)fclose(f);
    if (out_required != nullptr) {
        *out_required = total;
    }
    return over ? SPG_E_LIMIT : SPG_OK;
}

/* Render the full index (TOPK lines + "N more" pointer) into the store cache,
 * which is sized to hold the maximum so nothing is clipped, and mark it valid.
 * This is the one directory scan; repeated reads then serve from the cache. */
static void rebuild_index_cache(struct spg_mem_store *store) {
    const size_t count =
        collect_by_recency(store, g_collect_scratch, SPG_MEM_MAX_FILES);

    const size_t cap   = sizeof store->index_cache;
    size_t       used  = 0u;
    bool         trunc = false;

    const size_t shown = count < SPG_MEM_INDEX_TOPK ? count : SPG_MEM_INDEX_TOPK;
    for (size_t i = 0u; i < shown; i += 1u) {
        char line[SPG_MEM_SLUG_MAX + SPG_MEM_DESC_MAX + 8u];
        const int ln = snprintf(line, sizeof line, "- %s: %s\n",
                                g_collect_scratch[i].slug,
                                g_collect_scratch[i].desc);
        if (ln < 0) {
            continue;
        }
        const size_t n = (size_t)ln;
        if (used + n <= cap) {
            memcpy(store->index_cache + used, line, n);
            used += n;
        }
    }
    if (count > SPG_MEM_INDEX_TOPK) {
        trunc = true;
        char ptr[64];
        const int ln = snprintf(ptr, sizeof ptr, "- ... %zu more (memory list)\n",
                                count - SPG_MEM_INDEX_TOPK);
        if (ln > 0) {
            const size_t n = (size_t)ln;
            if (used + n <= cap) {
                memcpy(store->index_cache + used, ptr, n);
                used += n;
            }
        }
    }
    store->index_len       = used;
    store->index_truncated = trunc;
    store->index_valid     = true;
}

enum spg_status spg_mem_index(struct spg_mem_store *store, const size_t dst_cap,
                              char dst[], size_t *out_required,
                              bool *truncated) {
    if (store == nullptr || dst == nullptr || dst_cap == 0u) {
        return SPG_E_INVALID_ARG;
    }
    if (!store->index_valid) {
        rebuild_index_cache(store);
    }
    const size_t len  = store->index_len;
    const size_t copy = len < dst_cap - 1u ? len : dst_cap - 1u;
    memcpy(dst, store->index_cache, copy);
    dst[copy] = '\0';
    if (out_required != nullptr) {
        *out_required = len;
    }
    if (truncated != nullptr) {
        *truncated = store->index_truncated;
    }
    return len > dst_cap - 1u ? SPG_E_LIMIT : SPG_OK;
}

enum spg_status spg_mem_list(struct spg_mem_store *store, const size_t cap,
                             char slugs[][SPG_MEM_SLUG_MAX + 1u],
                             size_t *count) {
    if (store == nullptr || slugs == nullptr || count == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    /* Slug-sorted scratch (distinct from the recency scratch above). Same
     * single-threaded, non-reentrant contract as g_collect_scratch. */
    static char  all[SPG_MEM_MAX_FILES][SLUGBUF];
    const size_t total = collect_sorted(store, all, SPG_MEM_MAX_FILES);
    const size_t n     = total < cap ? total : cap;
    for (size_t i = 0u; i < n; i += 1u) {
        memcpy(slugs[i], all[i], SLUGBUF);
    }
    *count = n;
    return total > cap ? SPG_E_LIMIT : SPG_OK;
}

size_t spg_mem_directive(struct spg_mem_store *store, const char *slug,
                         const size_t budget_bytes, const size_t dst_cap,
                         char dst[]) {
    if (store == nullptr || slug == nullptr || dst == nullptr || dst_cap == 0u) {
        return 0u;
    }
    dst[0] = '\0';

    /* The index renders one "- <slug>: <description>\n" line per memory; the
     * directive is a slug's description, extracted without a new read path. */
    static char index[SPG_MEM_INDEX_CACHE_BYTES];
    if (spg_mem_index(store, sizeof index, index, nullptr, nullptr) != SPG_OK) {
        return 0u;
    }

    char prefix[SPG_MEM_SLUG_MAX + 4u];
    const int pn = snprintf(prefix, sizeof prefix, "- %s: ", slug);
    if (pn <= 0 || (size_t)pn >= sizeof prefix) {
        return 0u;
    }
    const char *line = index;
    while (*line != '\0') {
        const char *eol = strchr(line, '\n');
        const size_t line_n = eol != nullptr ? (size_t)(eol - line) : strlen(line);
        if (strncmp(line, prefix, (size_t)pn) == 0) {
            const char  *desc   = line + pn;
            const size_t desc_n = line_n - (size_t)pn;
            const size_t budget =
                budget_bytes > 0u ? budget_bytes : SPG_MEM_DESC_MAX;
            if (desc_n == 0u || desc_n > budget || desc_n + 1u > dst_cap) {
                return 0u; /* one slot, description-only, budget-capped */
            }
            memcpy(dst, desc, desc_n);
            dst[desc_n] = '\0';
            return desc_n;
        }
        if (eol == nullptr) {
            break;
        }
        line = eol + 1u;
    }
    return 0u;
}
