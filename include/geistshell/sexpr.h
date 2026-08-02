#ifndef GEISTSHELL_SEXPR_H
#define GEISTSHELL_SEXPR_H

#include "geistshell/status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPG_SEXPR_INVALID_INDEX UINT32_MAX

enum spg_sexpr_token_kind {
    SPG_SEXPR_TOKEN_LPAREN = 0,
    SPG_SEXPR_TOKEN_RPAREN,
    SPG_SEXPR_TOKEN_SYMBOL,
    SPG_SEXPR_TOKEN_STRING,
};

struct spg_text_span {
    size_t offset;
    size_t length;
};

struct spg_sexpr_token {
    enum spg_sexpr_token_kind kind;
    struct spg_text_span      span;
    size_t                    line;
    size_t                    column;
};

enum spg_sexpr_node_kind {
    SPG_SEXPR_NODE_LIST = 0,
    SPG_SEXPR_NODE_SYMBOL,
    SPG_SEXPR_NODE_STRING,
};

struct spg_sexpr_node {
    enum spg_sexpr_node_kind kind;
    struct spg_text_span     span;
    uint32_t                 parent;
    uint32_t                 first_child;
    uint32_t                 next_sibling;
    /* Tail of the child list, maintained during parsing so children append in
     * O(1) instead of re-walking the sibling chain. Equals first_child for a
     * single child and SPG_SEXPR_INVALID_INDEX when there are none. */
    uint32_t                 last_child;
};

struct spg_sexpr_error {
    enum spg_status status;
    size_t          offset;
    size_t          line;
    size_t          column;
};

[[nodiscard]] enum spg_status
spg_sexpr_tokenize(size_t input_n, const char input[],
                   size_t token_capacity,
                   struct spg_sexpr_token tokens[static token_capacity],
                   size_t *token_count, struct spg_sexpr_error *error);

[[nodiscard]] enum spg_status
spg_sexpr_parse(size_t token_n,
                const struct spg_sexpr_token tokens[],
                size_t node_capacity,
                struct spg_sexpr_node nodes[static node_capacity],
                size_t *node_count, struct spg_sexpr_error *error);

/* ---- Parse-tree accessors ----
 * Shared helpers for walking and reading a parsed node tree against its
 * source text. The DSL/config consumers all need these; centralizing them
 * here keeps one copy of the span/bounds and overflow-checked-integer logic. */

/* True when span lies fully within an input of input_n bytes. */
[[nodiscard]] bool spg_sexpr_span_valid(size_t input_n,
                                        struct spg_text_span span);

/* True when span (into input) equals the NUL-terminated text byte-for-byte. */
[[nodiscard]] bool spg_sexpr_span_eq_cstr(size_t input_n, const char input[],
                                          struct spg_text_span span,
                                          const char *text);

/* First child of node, or SPG_SEXPR_INVALID_INDEX when it has none. */
[[nodiscard]] uint32_t
spg_sexpr_first_child(const struct spg_sexpr_node nodes[static 1],
                      uint32_t node);

/* Second child of node, or SPG_SEXPR_INVALID_INDEX when fewer than two. */
[[nodiscard]] uint32_t
spg_sexpr_second_child(const struct spg_sexpr_node nodes[static 1],
                       uint32_t node);

/* Strip the surrounding quotes of a STRING node, returning its payload span in
 * *out. False (out untouched) when node is null, not a STRING, or shorter than
 * the two delimiting quotes. The single home for the (offset+1, length-2)
 * quote-strip contract the recommendation/config/chat parsers all need. */
[[nodiscard]] bool
spg_sexpr_string_payload_span(const struct spg_sexpr_node *node,
                              struct spg_text_span *out);

/* Parse the decimal digits of span into *out, rejecting empty spans,
 * non-digits (SPG_E_FORMAT), and values past UINT64_MAX (SPG_E_OVERFLOW). */
[[nodiscard]] enum spg_status
spg_sexpr_parse_uint64_span(size_t input_n, const char input[],
                            struct spg_text_span span, uint64_t *out);

[[nodiscard]] enum spg_status
spg_sexpr_parse_text(size_t input_n, const char input[],
                     size_t token_capacity,
                     struct spg_sexpr_token tokens[static token_capacity],
                     size_t node_capacity,
                     struct spg_sexpr_node nodes[static node_capacity],
                     size_t *token_count, size_t *node_count,
                     struct spg_sexpr_error *error);

/* ---- Bounded s-expression / text writer ----
 * Appends text and decimal-number tokens into a caller-provided buffer,
 * keeping it NUL-terminated after every append. This is the single place the
 * payload truncation contract lives, replacing the per-emitter append helpers
 * the policy gate, sim executor, and orchestrator each used to carry.
 *
 * Truncation policy: an append that would not fit (token bytes plus the
 * terminator exceed the remaining capacity) writes nothing, latches
 * `truncated`, and returns SPG_E_LIMIT. `used` then still holds the byte
 * length written before the refused append. Once an append has been refused,
 * `used` no longer grows. Callers stop on the first non-OK status and read
 * `used`/`truncated` to report how much was emitted. */
struct spg_sexpr_writer {
    char  *buffer;    /* caller-provided, NUL-terminated output buffer */
    size_t capacity;  /* total bytes of buffer, including the terminator */
    size_t used;      /* bytes written so far, excluding the terminator */
    bool   truncated; /* latched once an append is refused for space */
};

/* Bind writer to buffer (capacity bytes, capacity > 0) and reset it to the
 * empty string. */
void spg_sexpr_writer_init(struct spg_sexpr_writer *writer, size_t capacity,
                           char buffer[static capacity]);

/* Append a NUL-terminated string. */
[[nodiscard]] enum spg_status
spg_sexpr_writer_append_text(struct spg_sexpr_writer *writer, const char *text);

/* Append the decimal rendering of an unsigned 64-bit value. */
[[nodiscard]] enum spg_status
spg_sexpr_writer_append_u64(struct spg_sexpr_writer *writer, uint64_t value);

/* Append the decimal rendering of a size_t value. */
[[nodiscard]] enum spg_status
spg_sexpr_writer_append_size(struct spg_sexpr_writer *writer, size_t value);

#ifdef __cplusplus
}
#endif

#endif
