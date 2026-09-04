#include "geistshell/mem_command.h"

#include "geistshell/mem_store.h"
#include "geistshell/pref.h"
#include "geistshell/status.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEM_READ_CAP  (SPG_MEM_BODY_MAX + 1024u)
#define MEM_INDEX_CAP 16384u

static void usage(void) {
    fprintf(stderr,
            "usage: geistshell memory [--dir <path>] "
            "<list | read <slug> | save <slug> <description> | delete <slug>\n"
            "       | pref <key> <value> [--evidence "
            "asserted|repeated|correction] [--count <n>]\n"
            "       | profile>\n"
            "  save reads the memory body from stdin\n"
            "  pref records a user preference ONLY when the evidence justifies "
            "it (#28)\n");
}

int spg_memory_command(const int argc, char **argv) {
    const char *dir_flag  = nullptr;
    const char *evidence  = "repeated"; /* #28 */
    size_t      count     = 0u;
    const char *pos[4]    = {nullptr, nullptr, nullptr, nullptr};
    size_t      np        = 0u;
    for (int i = 0; i < argc; i += 1) {
        if (strcmp(argv[i], "--dir") == 0 && i + 1 < argc) {
            dir_flag = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--evidence") == 0 && i + 1 < argc) {
            evidence = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
            count = (size_t)strtoull(argv[++i], nullptr, 10);
            continue;
        }
        if (np < 4u) {
            pos[np++] = argv[i];
        }
    }
    if (np == 0u) {
        usage();
        return 2;
    }

    struct spg_mem_store store;
    if (spg_mem_store_open(&store, spg_mem_resolve_dir(dir_flag)) != SPG_OK) {
        fprintf(stderr, "memory: cannot open store directory\n");
        return 1;
    }
    const char *verb = pos[0];

    if (strcmp(verb, "list") == 0) {
        static char           idx[MEM_INDEX_CAP];
        bool                  truncated = false;
        const enum spg_status s =
            spg_mem_index(&store, sizeof idx, idx, nullptr, &truncated);
        if (s != SPG_OK && s != SPG_E_LIMIT) {
            fprintf(stderr, "memory: list failed\n");
            return 1;
        }
        fputs(idx, stdout);
        return 0;
    }

    if (strcmp(verb, "read") == 0) {
        if (np < 2u) {
            usage();
            return 2;
        }
        static char           buf[MEM_READ_CAP];
        const enum spg_status s =
            spg_mem_read(&store, pos[1], sizeof buf, buf, nullptr);
        if (s == SPG_E_NOT_FOUND) {
            fprintf(stderr, "memory: not found: %s\n", pos[1]);
            return 1;
        }
        if (s == SPG_E_INVALID_ARG) {
            fprintf(stderr, "memory: invalid slug\n");
            return 1;
        }
        fputs(buf, stdout);
        return 0;
    }

    if (strcmp(verb, "directive") == 0) {
        /* The one-line P6 injection for a slug (docs/LEARNING.md): what the
         * small-model auto-injection actually costs, vs the full `list` index. */
        if (np < 2u) {
            usage();
            return 2;
        }
        char         line[SPG_MEM_DESC_MAX + 1u];
        const size_t n =
            spg_mem_directive(&store, pos[1], 0u, sizeof line, line);
        if (n == 0u) {
            return 1;
        }
        puts(line);
        return 0;
    }

    if (strcmp(verb, "save") == 0) {
        if (np < 3u) {
            usage();
            return 2;
        }
        static char body[SPG_MEM_BODY_MAX + 1u];
        size_t      used = 0u;
        size_t      r;
        char        chunk[4096];
        while ((r = fread(chunk, 1u, sizeof chunk, stdin)) > 0u) {
            if (used + r > SPG_MEM_BODY_MAX) {
                fprintf(stderr, "memory: body too large\n");
                return 1;
            }
            memcpy(body + used, chunk, r);
            used += r;
        }
        body[used] = '\0';
        const enum spg_status s = spg_mem_save(&store, pos[1], pos[2], body);
        if (s == SPG_E_INVALID_ARG) {
            fprintf(stderr, "memory: invalid slug or description\n");
            return 1;
        }
        if (s == SPG_E_LIMIT) {
            fprintf(stderr, "memory: store full or input too large\n");
            return 1;
        }
        if (s != SPG_OK) {
            fprintf(stderr, "memory: save failed\n");
            return 1;
        }
        printf("saved %s\n", pos[1]);
        return 0;
    }

    if (strcmp(verb, "pref") == 0) {
        /* #28: write-on-evidence. The preference is recorded only when the
         * world showed it — a repeated choice (--count >= 2) or a correction
         * (--count >= 1); a model self-assertion (--evidence asserted) never
         * writes. */
        if (np < 3u) {
            usage();
            return 2;
        }
        enum spg_pref_evidence ev = SPG_PREF_EVIDENCE_REPEATED;
        if (strcmp(evidence, "asserted") == 0) {
            ev = SPG_PREF_EVIDENCE_ASSERTED;
        } else if (strcmp(evidence, "correction") == 0) {
            ev = SPG_PREF_EVIDENCE_CORRECTION;
        } else if (strcmp(evidence, "repeated") != 0) {
            fprintf(stderr, "memory: --evidence must be asserted|repeated|"
                            "correction\n");
            return 2;
        }
        bool                  wrote = false;
        const enum spg_status s =
            spg_pref_record(&store, pos[1], pos[2], ev, count, &wrote);
        if (s == SPG_E_INVALID_ARG) {
            fprintf(stderr, "memory: invalid pref key or value\n");
            return 1;
        }
        if (s != SPG_OK) {
            fprintf(stderr, "memory: pref save failed\n");
            return 1;
        }
        printf("{\"pref\":\"%s\",\"wrote\":%s}\n", pos[1],
               wrote ? "true" : "false");
        /* Insufficient evidence is a non-zero exit so a caller can tell "not
         * written" from "written" without parsing — declining is correct, but
         * it is not the write the caller asked to attempt. */
        return wrote ? 0 : 1;
    }

    if (strcmp(verb, "profile") == 0) {
        char         line[SPG_MEM_DESC_MAX + 32u];
        const size_t n = spg_pref_render(&store, 0u, sizeof line, line);
        if (n == 0u) {
            return 1; /* no preferences recorded yet */
        }
        puts(line);
        return 0;
    }

    if (strcmp(verb, "delete") == 0) {
        if (np < 2u) {
            usage();
            return 2;
        }
        const enum spg_status s = spg_mem_delete(&store, pos[1]);
        if (s == SPG_E_NOT_FOUND) {
            fprintf(stderr, "memory: not found: %s\n", pos[1]);
            return 1;
        }
        if (s != SPG_OK) {
            fprintf(stderr, "memory: delete failed\n");
            return 1;
        }
        printf("deleted %s\n", pos[1]);
        return 0;
    }

    usage();
    return 2;
}
