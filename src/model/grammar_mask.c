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

bool spg_kind_prefix_ok(const char *emitted, const char *piece) {
    if (emitted == nullptr) {
        return false;
    }
    char         buf[64];
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
    for (size_t i = 0u; i < KIND_COUNT; i += 1u) {
        const char *name = spg_action_kind_to_string(ALL_KINDS[i]);
        /* cmp is a prefix of name iff their first `len` bytes match; a name
         * shorter than `len` has its '\0' compared against cmp's byte and
         * mismatches, which correctly rejects overshoot. An all-space buffer
         * (len 0) matches every name — still a live prefix. */
        if (strncmp(name, cmp, len) == 0) {
            return true;
        }
    }
    return false;
}

bool spg_kind_complete(const char *emitted) {
    if (emitted == nullptr) {
        return false;
    }
    const char *cmp = skip_spaces(emitted);
    if (cmp[0] == '\0') {
        return false;
    }
    for (size_t i = 0u; i < KIND_COUNT; i += 1u) {
        if (strcmp(spg_action_kind_to_string(ALL_KINDS[i]), cmp) == 0) {
            return true;
        }
    }
    return false;
}
