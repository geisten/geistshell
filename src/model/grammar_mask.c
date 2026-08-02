#include "geistshell/grammar_mask.h"

#include "geistshell/policy.h"

#include <string.h>

/* Every action kind that parse_action_kind accepts. Their canonical names come
 * from spg_action_kind_to_string, so this list is the enum, not a second copy
 * of the strings. */
static const enum spg_action_kind ALL_KINDS[] = {
    SPG_ACTION_LOCAL_SHELL, SPG_ACTION_SSH_AUTH_PROBE, SPG_ACTION_SIMULATOR,
    SPG_ACTION_MEMORY_SAVE, SPG_ACTION_MEMORY_DELETE, SPG_ACTION_MEMORY_READ,
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
    return spg_choice_complete(names, spg_kind_names(names, KIND_COUNT), emitted);
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
#define BUREAU_OK ") (cost 1) (uses_network false) (confidence_bp 5000) "
#define BUREAU_NET ") (cost 1) (uses_network true) (confidence_bp 5000) "

static const struct spg_scaffold_seg SEG_FINISH[] = {
    LIT(") (reason \""), STR, LIT("\"))")};
static const struct spg_scaffold_seg SEG_SIMULATOR[] = {
    LIT(") (capability \""), CAP, LIT("\"" BUREAU_OK "(reason \""), STR,
    LIT("\"))")};
static const struct spg_scaffold_seg SEG_LOCAL_SHELL[] = {
    LIT(") (capability \""), CAP, LIT("\"" BUREAU_OK "(command \""), STR,
    LIT("\") (reason \""), STR, LIT("\"))")};
static const struct spg_scaffold_seg SEG_SSH[] = {
    LIT(") (capability \""), CAP, LIT("\"" BUREAU_NET "(target \""), STR,
    LIT("\") (reason \""), STR, LIT("\"))")};
static const struct spg_scaffold_seg SEG_MEM_SAVE[] = {
    LIT(") (capability \""), CAP, LIT("\"" BUREAU_OK "(slug \""), STR,
    LIT("\") (description \""), STR, LIT("\") (body \""), STR,
    LIT("\") (reason \""), STR, LIT("\"))")};
static const struct spg_scaffold_seg SEG_MEM_SLUG[] = {
    LIT(") (capability \""), CAP, LIT("\"" BUREAU_OK "(slug \""), STR,
    LIT("\") (reason \""), STR, LIT("\"))")};

size_t spg_scaffold_for_kind(enum spg_action_kind kind,
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
