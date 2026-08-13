#include "geistshell/grammar_mask.h"

#include "geistshell/policy.h"

#include <string.h>

/* Every action kind that parse_action_kind accepts. Their canonical names come
 * from spg_action_kind_to_string, so this list is the enum, not a second copy
 * of the strings. */
static const enum spg_action_kind ALL_KINDS[] = {
    SPG_ACTION_LOCAL_SHELL, SPG_ACTION_SSH_AUTH_PROBE, SPG_ACTION_SIMULATOR,
    SPG_ACTION_MEMORY_SAVE, SPG_ACTION_MEMORY_DELETE,  SPG_ACTION_MEMORY_READ,
    SPG_ACTION_FINISH,
};
static const size_t KIND_COUNT = sizeof ALL_KINDS / sizeof ALL_KINDS[0];

/* The tokenizer detokenizes a SentencePiece word-boundary (U+2581) to a leading
 * ASCII space, so the first kind token reads as " local", not "local". Kind
 * names never start with space and the s-expression tolerates the separator, so
 * we compare against the text past any leading spaces. */
static const char *skip_spaces(const char *s) {
    while (*s == ' ' || *s == '\t') {
        s += 1;
    }
    return s;
}

bool spg_choice_prefix_ok(const char *const *names, const size_t n,
                          const char *emitted, const char *piece) {
    if (names == nullptr || emitted == nullptr) {
        return false;
    }
    char         buf[128];
    const size_t el = strlen(emitted);
    const size_t pl = piece == nullptr ? 0u : strlen(piece);
    if (el + pl >= sizeof buf) {
        return false;
    }
    memcpy(buf, emitted, el);
    memcpy(buf + el, piece == nullptr ? "" : piece, pl);
    buf[el + pl]     = '\0';
    const char  *cmp = skip_spaces(buf);
    const size_t len = strlen(cmp);
    for (size_t i = 0u; i < n; i += 1u) {
        if (names[i] == nullptr) {
            continue;
        }
        /* cmp is a prefix of names[i] iff their first `len` bytes match; a name
         * shorter than `len` has its '\0' compared against cmp's byte and
         * mismatches, which correctly rejects overshoot. An all-space buffer
         * (len 0) matches every name — still a live prefix. */
        if (strncmp(names[i], cmp, len) == 0) {
            return true;
        }
    }
    return false;
}

bool spg_choice_complete(const char *const *names, const size_t n,
                         const char *emitted) {
    if (names == nullptr || emitted == nullptr) {
        return false;
    }
    const char *cmp = skip_spaces(emitted);
    if (cmp[0] == '\0') {
        return false;
    }
    for (size_t i = 0u; i < n; i += 1u) {
        if (names[i] != nullptr && strcmp(names[i], cmp) == 0) {
            return true;
        }
    }
    return false;
}

size_t spg_kind_names(const char **out, const size_t cap) {
    size_t n = 0u;
    for (size_t i = 0u; i < KIND_COUNT && n < cap; i += 1u) {
        out[n] = spg_action_kind_to_string(ALL_KINDS[i]);
        n += 1u;
    }
    return n;
}

bool spg_kind_prefix_ok(const char *emitted, const char *piece) {
    const char *names[KIND_COUNT];
    return spg_choice_prefix_ok(names, spg_kind_names(names, KIND_COUNT),
                                emitted, piece);
}

bool spg_kind_complete(const char *emitted) {
    const char *names[KIND_COUNT];
    return spg_choice_complete(names, spg_kind_names(names, KIND_COUNT),
                               emitted);
}

bool spg_kind_from_text(const char *emitted, enum spg_action_kind *out) {
    if (emitted == nullptr || out == nullptr) {
        return false;
    }
    const char *cmp = skip_spaces(emitted);
    for (size_t i = 0u; i < KIND_COUNT; i += 1u) {
        if (strcmp(spg_action_kind_to_string(ALL_KINDS[i]), cmp) == 0) {
            *out = ALL_KINDS[i];
            return true;
        }
    }
    return false;
}

/* Scaffolds, positioned right after "(recommend (kind <name>". A nullptr
 * literal is a free-decoded string value. The cost/uses_network/confidence_bp
 * defaults are baked in; uses_network is fixed per kind by kind_fields_match
 * (true only for ssh_auth_probe). Field sets mirror required_fields_seen +
 * kind_fields_match in recommendation.c. */
#define LIT(s) {.kind = SPG_SCAFFOLD_LITERAL, .literal = (s)}
#define STR {.kind = SPG_SCAFFOLD_STRING}
#define CAP {.kind = SPG_SCAFFOLD_CAPABILITY}
#define NUM {.kind = SPG_SCAFFOLD_NUMBER}
#define BUREAU_OK ") (cost 1) (uses_network false) (confidence_bp 5000) "
#define BUREAU_NET ") (cost 1) (uses_network true) (confidence_bp 5000) "

static const struct spg_scaffold_seg SEG_FINISH[] = {LIT(") (reason \""), STR,
                                                     LIT("\"))")};

/* Machine actions carry a target and no command — the shape that keeps a shell
 * string out of the action space (#66, #75). */
static const struct spg_scaffold_seg SEG_MACHINE[] = {
    LIT(") (capability \""),
    CAP,
    LIT("\"" BUREAU_OK "(target \""),
    STR,
    LIT("\") (reason \""),
    STR,
    LIT("\"))")};
/* A device write: which channel, and the number the model chose. The value is
 * the only free number in the whole grammar — everything else the decoder fills
 * is a string. */
static const struct spg_scaffold_seg SEG_DEVICE[] = {
    LIT(") (capability \""),
    CAP,
    LIT("\"" BUREAU_OK "(target \""),
    STR,
    LIT("\") (value "),
    NUM,
    LIT(") (reason \""),
    STR,
    LIT("\"))")};
static const struct spg_scaffold_seg SEG_SIMULATOR[] = {
    LIT(") (capability \""), CAP, LIT("\"" BUREAU_OK "(reason \""), STR,
    LIT("\"))")};
static const struct spg_scaffold_seg SEG_LOCAL_SHELL[] = {
    LIT(") (capability \""),
    CAP,
    LIT("\"" BUREAU_OK "(command \""),
    STR,
    LIT("\") (reason \""),
    STR,
    LIT("\"))")};
static const struct spg_scaffold_seg SEG_SSH[] = {
    LIT(") (capability \""),
    CAP,
    LIT("\"" BUREAU_NET "(target \""),
    STR,
    LIT("\") (reason \""),
    STR,
    LIT("\"))")};
static const struct spg_scaffold_seg SEG_MEM_SAVE[] = {
    LIT(") (capability \""),
    CAP,
    LIT("\"" BUREAU_OK "(slug \""),
    STR,
    LIT("\") (description \""),
    STR,
    LIT("\") (body \""),
    STR,
    LIT("\") (reason \""),
    STR,
    LIT("\"))")};
static const struct spg_scaffold_seg SEG_MEM_SLUG[] = {
    LIT(") (capability \""),
    CAP,
    LIT("\"" BUREAU_OK "(slug \""),
    STR,
    LIT("\") (reason \""),
    STR,
    LIT("\"))")};

/* The action kinds one policy capability kind covers. Memory is one capability
 * over three kinds, so the mask needs an entry per kind. */
static size_t kinds_for_cap(const enum spg_policy_capability_kind cap,
                            enum spg_action_kind out[static 3]) {
    switch (cap) {
    /* Machine kinds were missing here since #66 — the constrained decoder
     * could not offer them at all, which -Wswitch only surfaced when #75 added
     * a third. A closed enum is only closed if every switch over it is. */
    case SPG_POLICY_CAP_MACHINE_PROCESS:
        out[0] = SPG_ACTION_MACHINE_PAUSE;
        out[1] = SPG_ACTION_MACHINE_RESUME;
        return 2u;
    case SPG_POLICY_CAP_DEVICE:
        out[0] = SPG_ACTION_DEVICE_WRITE;
        return 1u;
    case SPG_POLICY_CAP_LOCAL_SHELL:
        out[0] = SPG_ACTION_LOCAL_SHELL;
        return 1u;
    case SPG_POLICY_CAP_SSH_AUTH_PROBE:
        out[0] = SPG_ACTION_SSH_AUTH_PROBE;
        return 1u;
    case SPG_POLICY_CAP_SIMULATOR:
        out[0] = SPG_ACTION_SIMULATOR;
        return 1u;
    case SPG_POLICY_CAP_MEMORY:
        out[0] = SPG_ACTION_MEMORY_SAVE;
        out[1] = SPG_ACTION_MEMORY_DELETE;
        out[2] = SPG_ACTION_MEMORY_READ;
        return 3u;
    }
    return 0u;
}

size_t spg_model_capabilities_from_policy(
    const struct spg_policy_config *policy, const size_t text_n,
    const char text[], const size_t name_buf_cap, char name_buf[],
    const size_t out_cap, struct spg_model_capability out[]) {
    if (policy == nullptr || text == nullptr || name_buf == nullptr ||
        out == nullptr || name_buf_cap == 0u) {
        return 0u;
    }
    size_t n    = 0u;
    size_t used = 0u; /* bytes taken in name_buf, including terminators */
    for (size_t i = 0u; i < policy->capability_count && n < out_cap; i += 1u) {
        const struct spg_policy_capability *cap = &policy->capabilities[i];
        if (!cap->enabled) {
            continue;
        }
        const struct spg_text_span span = cap->name;
        if (span.offset > text_n || span.length > text_n - span.offset ||
            span.length == 0u) {
            continue;
        }
        if (span.length + 1u > name_buf_cap - used) {
            break; /* names exhausted: a narrower mask, still valid */
        }
        char *name = name_buf + used;
        memcpy(name, text + span.offset, span.length);
        name[span.length] = '\0';
        used += span.length + 1u;

        enum spg_action_kind kinds[3];
        const size_t         nk = kinds_for_cap(cap->kind, kinds);
        for (size_t k = 0u; k < nk && n < out_cap; k += 1u) {
            out[n].name = name;
            out[n].kind = kinds[k];
            n += 1u;
        }
    }
    return n;
}

size_t spg_scaffold_for_kind(enum spg_action_kind            kind,
                             const struct spg_scaffold_seg **out) {
    if (out == nullptr) {
        return 0u;
    }
#define RET(arr)                                                               \
    do {                                                                       \
        *out = (arr);                                                          \
        return sizeof(arr) / sizeof(arr)[0];                                   \
    } while (0)
    switch (kind) {
    case SPG_ACTION_MACHINE_PAUSE:
    case SPG_ACTION_MACHINE_RESUME:
        RET(SEG_MACHINE);
    case SPG_ACTION_DEVICE_WRITE:
        RET(SEG_DEVICE);
    case SPG_ACTION_FINISH:
        RET(SEG_FINISH);
    case SPG_ACTION_SIMULATOR:
        RET(SEG_SIMULATOR);
    case SPG_ACTION_LOCAL_SHELL:
        RET(SEG_LOCAL_SHELL);
    case SPG_ACTION_SSH_AUTH_PROBE:
        RET(SEG_SSH);
    case SPG_ACTION_MEMORY_SAVE:
        RET(SEG_MEM_SAVE);
    case SPG_ACTION_MEMORY_DELETE:
    case SPG_ACTION_MEMORY_READ:
        RET(SEG_MEM_SLUG);
    }
#undef RET
    *out = nullptr;
    return 0u;
}
