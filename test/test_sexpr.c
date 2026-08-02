#include "geistshell/sexpr.h"

#include <stdio.h>
#include <string.h>

static int parse_text(const char *text, size_t token_capacity,
                      struct spg_sexpr_token tokens[static token_capacity],
                      size_t node_capacity,
                      struct spg_sexpr_node nodes[static node_capacity],
                      size_t *token_count, size_t *node_count,
                      struct spg_sexpr_error *error) {
    const enum spg_status status =
        spg_sexpr_parse_text(strlen(text), text, token_capacity, tokens,
                             node_capacity, nodes, token_count, node_count,
                             error);
    return status == SPG_OK ? 0 : 1;
}

static int test_parse_simple_tree(void) {
    const char              *text = "(run (model \"m.gguf\") (seed 7))";
    struct spg_sexpr_token   tokens[16];
    struct spg_sexpr_node    nodes[16];
    struct spg_sexpr_error   error = {};
    size_t                   token_count = 0u;
    size_t                   node_count  = 0u;

    if (parse_text(text, 16u, tokens, 16u, nodes, &token_count, &node_count,
                   &error) != 0) {
        return 1;
    }
    if (token_count != 11u || node_count != 8u) {
        return 1;
    }
    if (nodes[0].kind != SPG_SEXPR_NODE_LIST ||
        nodes[0].parent != SPG_SEXPR_INVALID_INDEX ||
        nodes[0].first_child != 1u) {
        return 1;
    }
    if (nodes[1].kind != SPG_SEXPR_NODE_SYMBOL ||
        nodes[1].next_sibling != 2u) {
        return 1;
    }
    if (nodes[2].kind != SPG_SEXPR_NODE_LIST ||
        nodes[2].parent != 0u) {
        return 1;
    }
    if (nodes[4].kind != SPG_SEXPR_NODE_STRING) {
        return 1;
    }
    return 0;
}

static int test_comments_and_strings(void) {
    const char            *text = "; comment\n(policy (name \"lab\\\"one\"))";
    struct spg_sexpr_token tokens[16];
    struct spg_sexpr_node  nodes[16];
    struct spg_sexpr_error error = {};
    size_t                 token_count = 0u;
    size_t                 node_count  = 0u;

    if (parse_text(text, 16u, tokens, 16u, nodes, &token_count, &node_count,
                   &error) != 0) {
        return 1;
    }
    if (token_count != 7u || node_count != 5u) {
        return 1;
    }
    if (tokens[0].line != 2u || tokens[0].column != 1u) {
        return 1;
    }
    return 0;
}

static int test_multiple_top_level_forms(void) {
    const char            *text = "(a)(b c)";
    struct spg_sexpr_token tokens[16];
    struct spg_sexpr_node  nodes[16];
    struct spg_sexpr_error error = {};
    size_t                 token_count = 0u;
    size_t                 node_count  = 0u;

    if (parse_text(text, 16u, tokens, 16u, nodes, &token_count, &node_count,
                   &error) != 0) {
        return 1;
    }
    if (node_count != 5u) {
        return 1;
    }
    if (nodes[0].next_sibling != SPG_SEXPR_INVALID_INDEX ||
        nodes[2].parent != SPG_SEXPR_INVALID_INDEX) {
        return 1;
    }
    return 0;
}

static int test_empty_input(void) {
    const char            *text = "";
    struct spg_sexpr_token tokens[1];
    struct spg_sexpr_node  nodes[1];
    struct spg_sexpr_error error = {};
    size_t                 token_count = 99u;
    size_t                 node_count  = 99u;

    if (spg_sexpr_parse_text(strlen(text), text, 1u, tokens, 1u, nodes,
                             &token_count, &node_count, &error) != SPG_OK) {
        return 1;
    }
    return token_count == 0u && node_count == 0u ? 0 : 1;
}

static int test_token_capacity_limit(void) {
    const char            *text = "(a b c)";
    struct spg_sexpr_token tokens[3];
    struct spg_sexpr_node  nodes[8];
    struct spg_sexpr_error error = {};
    size_t                 token_count = 0u;
    size_t                 node_count  = 0u;

    const enum spg_status status =
        spg_sexpr_parse_text(strlen(text), text, 3u, tokens, 8u, nodes,
                             &token_count, &node_count, &error);
    if (status != SPG_E_LIMIT || error.status != SPG_E_LIMIT) {
        return 1;
    }
    return node_count == 0u ? 0 : 1;
}

static int test_node_capacity_limit(void) {
    const char            *text = "(a b c)";
    struct spg_sexpr_token tokens[8];
    struct spg_sexpr_node  nodes[3];
    struct spg_sexpr_error error = {};
    size_t                 token_count = 0u;
    size_t                 node_count  = 0u;

    const enum spg_status status =
        spg_sexpr_parse_text(strlen(text), text, 8u, tokens, 3u, nodes,
                             &token_count, &node_count, &error);
    if (status != SPG_E_LIMIT || error.status != SPG_E_LIMIT) {
        return 1;
    }
    return node_count == 0u ? 0 : 1;
}

static int test_unbalanced_close(void) {
    const char            *text = "(a))";
    struct spg_sexpr_token tokens[8];
    struct spg_sexpr_node  nodes[8];
    struct spg_sexpr_error error = {};
    size_t                 token_count = 0u;
    size_t                 node_count  = 0u;

    const enum spg_status status =
        spg_sexpr_parse_text(strlen(text), text, 8u, tokens, 8u, nodes,
                             &token_count, &node_count, &error);
    return status == SPG_E_FORMAT && node_count == 0u ? 0 : 1;
}

static int test_unbalanced_open(void) {
    const char            *text = "(a";
    struct spg_sexpr_token tokens[8];
    struct spg_sexpr_node  nodes[8];
    struct spg_sexpr_error error = {};
    size_t                 token_count = 0u;
    size_t                 node_count  = 0u;

    const enum spg_status status =
        spg_sexpr_parse_text(strlen(text), text, 8u, tokens, 8u, nodes,
                             &token_count, &node_count, &error);
    return status == SPG_E_FORMAT && node_count == 0u ? 0 : 1;
}

static int test_invalid_escape(void) {
    const char            *text = "(a \"bad\\x\")";
    struct spg_sexpr_token tokens[8];
    struct spg_sexpr_node  nodes[8];
    struct spg_sexpr_error error = {};
    size_t                 token_count = 0u;
    size_t                 node_count  = 0u;

    const enum spg_status status =
        spg_sexpr_parse_text(strlen(text), text, 8u, tokens, 8u, nodes,
                             &token_count, &node_count, &error);
    return status == SPG_E_FORMAT && node_count == 0u ? 0 : 1;
}

static int test_invalid_args(void) {
    const char             input[1] = {0};
    struct spg_sexpr_token tokens[1];
    struct spg_sexpr_node  nodes[1];
    struct spg_sexpr_error error = {};

    if (spg_sexpr_tokenize(0u, input, 1u, tokens, nullptr, &error) !=
        SPG_E_INVALID_ARG) {
        return 1;
    }
    if (spg_sexpr_parse(0u, tokens, 1u, nodes, nullptr, &error) !=
        SPG_E_INVALID_ARG) {
        return 1;
    }
    return 0;
}

static int test_writer_appends_tokens(void) {
    char                    buffer[64];
    struct spg_sexpr_writer writer;
    spg_sexpr_writer_init(&writer, sizeof buffer, buffer);

    if (writer.used != 0u || writer.truncated || buffer[0] != '\0') {
        return 1;
    }
    if (spg_sexpr_writer_append_text(&writer, "(x ") != SPG_OK ||
        spg_sexpr_writer_append_u64(&writer, 42u) != SPG_OK ||
        spg_sexpr_writer_append_text(&writer, " ") != SPG_OK ||
        spg_sexpr_writer_append_size(&writer, 7u) != SPG_OK ||
        spg_sexpr_writer_append_text(&writer, ")") != SPG_OK) {
        return 1;
    }
    if (strcmp(buffer, "(x 42 7)") != 0 || writer.used != 8u ||
        writer.truncated) {
        return 1;
    }
    return 0;
}

static int test_writer_truncates_and_latches(void) {
    char                    buffer[8];
    struct spg_sexpr_writer writer;
    spg_sexpr_writer_init(&writer, sizeof buffer, buffer);

    /* "abcdef" (6) plus the terminator fits exactly in 8 bytes minus the
     * second append. */
    if (spg_sexpr_writer_append_text(&writer, "abcdef") != SPG_OK ||
        writer.used != 6u || writer.truncated) {
        return 1;
    }
    /* "gh" would need 2 more bytes but only 1 remains before the terminator;
     * the append writes nothing and latches truncation. */
    if (spg_sexpr_writer_append_text(&writer, "gh") != SPG_E_LIMIT ||
        writer.used != 6u || !writer.truncated ||
        strcmp(buffer, "abcdef") != 0) {
        return 1;
    }
    /* Once truncated, even a fitting append is refused and leaves used fixed. */
    if (spg_sexpr_writer_append_text(&writer, "i") != SPG_E_LIMIT ||
        writer.used != 6u || strcmp(buffer, "abcdef") != 0) {
        return 1;
    }
    return 0;
}

static int test_writer_invalid_args(void) {
    char                    buffer[8];
    struct spg_sexpr_writer writer;
    spg_sexpr_writer_init(&writer, sizeof buffer, buffer);

    if (spg_sexpr_writer_append_text(&writer, nullptr) != SPG_E_INVALID_ARG) {
        return 1;
    }
    if (spg_sexpr_writer_append_text(nullptr, "x") != SPG_E_INVALID_ARG) {
        return 1;
    }
    return 0;
}

static int test_accessor_children(void) {
    const char            *text = "(a b c)";
    struct spg_sexpr_token tokens[8];
    struct spg_sexpr_node  nodes[8];
    struct spg_sexpr_error error       = {};
    size_t                 token_count = 0u;
    size_t                 node_count  = 0u;

    if (parse_text(text, 8u, tokens, 8u, nodes, &token_count, &node_count,
                   &error) != 0) {
        return 1;
    }
    /* node 0 is the list; its children are symbols a(1) b(2) c(3). */
    if (spg_sexpr_first_child(nodes, 0u) != 1u ||
        spg_sexpr_second_child(nodes, 0u) != 2u) {
        return 1;
    }
    /* A leaf symbol has no children; second_child is invalid when there is
     * only one. */
    if (spg_sexpr_first_child(nodes, 1u) != SPG_SEXPR_INVALID_INDEX ||
        spg_sexpr_second_child(nodes, 3u) != SPG_SEXPR_INVALID_INDEX) {
        return 1;
    }
    return 0;
}

static int test_accessor_sibling_order(void) {
    /* A flat list with several children stresses the O(1) append cursor:
     * walking first_child then next_sibling must yield a, b, c, d, e in
     * order. */
    const char            *text = "(a b c d e)";
    struct spg_sexpr_token tokens[16];
    struct spg_sexpr_node  nodes[16];
    struct spg_sexpr_error error       = {};
    size_t                 token_count = 0u;
    size_t                 node_count  = 0u;
    const size_t           input_n     = strlen(text);

    if (parse_text(text, 16u, tokens, 16u, nodes, &token_count, &node_count,
                   &error) != 0) {
        return 1;
    }
    const char *const expected[] = {"a", "b", "c", "d", "e"};
    uint32_t          child      = spg_sexpr_first_child(nodes, 0u);
    for (size_t i = 0u; i < 5u; i += 1u) {
        if (child == SPG_SEXPR_INVALID_INDEX) {
            return 1;
        }
        if (!spg_sexpr_span_eq_cstr(input_n, text, nodes[child].span,
                                    expected[i])) {
            return 1;
        }
        child = nodes[child].next_sibling;
    }
    /* Exactly five children, no more. */
    return child == SPG_SEXPR_INVALID_INDEX ? 0 : 1;
}

static int test_span_valid(void) {
    const size_t input_n = 10u;
    /* Fully inside, and flush against the end. */
    if (!spg_sexpr_span_valid(input_n,
                              (struct spg_text_span){.offset = 2u, .length = 3u}) ||
        !spg_sexpr_span_valid(input_n,
                              (struct spg_text_span){.offset = 7u, .length = 3u})) {
        return 1;
    }
    /* Zero-length is valid, even at the very end. */
    if (!spg_sexpr_span_valid(input_n,
                              (struct spg_text_span){.offset = 10u, .length = 0u})) {
        return 1;
    }
    /* Offset past the end, and length that runs past the end, are invalid. */
    if (spg_sexpr_span_valid(input_n,
                             (struct spg_text_span){.offset = 11u, .length = 0u}) ||
        spg_sexpr_span_valid(input_n,
                             (struct spg_text_span){.offset = 8u, .length = 3u})) {
        return 1;
    }
    return 0;
}

static int test_span_eq_cstr(void) {
    const char  *text    = "kind capability";
    const size_t input_n = strlen(text);
    const struct spg_text_span kind = {.offset = 0u, .length = 4u};
    const struct spg_text_span cap  = {.offset = 5u, .length = 10u};

    if (!spg_sexpr_span_eq_cstr(input_n, text, kind, "kind") ||
        !spg_sexpr_span_eq_cstr(input_n, text, cap, "capability")) {
        return 1;
    }
    /* Length mismatch (prefix), content mismatch, and nullptr arguments all
     * compare false rather than reading out of bounds. */
    if (spg_sexpr_span_eq_cstr(input_n, text, kind, "kin") ||
        spg_sexpr_span_eq_cstr(input_n, text, kind, "kine") ||
        spg_sexpr_span_eq_cstr(input_n, nullptr, kind, "kind") ||
        spg_sexpr_span_eq_cstr(input_n, text, kind, nullptr)) {
        return 1;
    }
    /* Out-of-bounds span is rejected, not dereferenced. */
    if (spg_sexpr_span_eq_cstr(
            input_n, text, (struct spg_text_span){.offset = 20u, .length = 4u},
            "kind")) {
        return 1;
    }
    return 0;
}

static int test_parse_uint64_span(void) {
    const char  *text    = "0 18446744073709551615 18446744073709551616 12x";
    const size_t input_n = strlen(text);
    uint64_t     out     = 99u;

    /* Zero. */
    if (spg_sexpr_parse_uint64_span(
            input_n, text, (struct spg_text_span){.offset = 0u, .length = 1u},
            &out) != SPG_OK ||
        out != 0u) {
        return 1;
    }
    /* UINT64_MAX exactly. */
    if (spg_sexpr_parse_uint64_span(
            input_n, text, (struct spg_text_span){.offset = 2u, .length = 20u},
            &out) != SPG_OK ||
        out != UINT64_MAX) {
        return 1;
    }
    /* One past UINT64_MAX overflows. */
    if (spg_sexpr_parse_uint64_span(
            input_n, text, (struct spg_text_span){.offset = 23u, .length = 20u},
            &out) != SPG_E_OVERFLOW) {
        return 1;
    }
    /* A non-digit byte is a format error. */
    if (spg_sexpr_parse_uint64_span(
            input_n, text, (struct spg_text_span){.offset = 44u, .length = 3u},
            &out) != SPG_E_FORMAT) {
        return 1;
    }
    /* Empty span, nullptr out, and out-of-bounds span are invalid arguments. */
    if (spg_sexpr_parse_uint64_span(
            input_n, text, (struct spg_text_span){.offset = 0u, .length = 0u},
            &out) != SPG_E_INVALID_ARG ||
        spg_sexpr_parse_uint64_span(
            input_n, text, (struct spg_text_span){.offset = 0u, .length = 1u},
            nullptr) != SPG_E_INVALID_ARG ||
        spg_sexpr_parse_uint64_span(
            input_n, text, (struct spg_text_span){.offset = 99u, .length = 1u},
            &out) != SPG_E_INVALID_ARG) {
        return 1;
    }
    return 0;
}

static int test_writer_number_truncation(void) {
    /* Capacity 4 holds "12" plus terminator with one byte to spare, but a
     * three-digit number does not fit and must latch truncation while leaving
     * the buffer intact. */
    char                    buffer[4];
    struct spg_sexpr_writer writer;
    spg_sexpr_writer_init(&writer, sizeof buffer, buffer);

    if (spg_sexpr_writer_append_u64(&writer, 12u) != SPG_OK ||
        writer.used != 2u || strcmp(buffer, "12") != 0) {
        return 1;
    }
    if (spg_sexpr_writer_append_size(&writer, 345u) != SPG_E_LIMIT ||
        !writer.truncated || writer.used != 2u || strcmp(buffer, "12") != 0) {
        return 1;
    }
    return 0;
}

int main(void) {
    if (test_parse_simple_tree() != 0) {
        fprintf(stderr, "test_parse_simple_tree failed\n");
        return 1;
    }
    if (test_comments_and_strings() != 0) {
        fprintf(stderr, "test_comments_and_strings failed\n");
        return 1;
    }
    if (test_multiple_top_level_forms() != 0) {
        fprintf(stderr, "test_multiple_top_level_forms failed\n");
        return 1;
    }
    if (test_empty_input() != 0) {
        fprintf(stderr, "test_empty_input failed\n");
        return 1;
    }
    if (test_token_capacity_limit() != 0) {
        fprintf(stderr, "test_token_capacity_limit failed\n");
        return 1;
    }
    if (test_node_capacity_limit() != 0) {
        fprintf(stderr, "test_node_capacity_limit failed\n");
        return 1;
    }
    if (test_unbalanced_close() != 0) {
        fprintf(stderr, "test_unbalanced_close failed\n");
        return 1;
    }
    if (test_unbalanced_open() != 0) {
        fprintf(stderr, "test_unbalanced_open failed\n");
        return 1;
    }
    if (test_invalid_escape() != 0) {
        fprintf(stderr, "test_invalid_escape failed\n");
        return 1;
    }
    if (test_invalid_args() != 0) {
        fprintf(stderr, "test_invalid_args failed\n");
        return 1;
    }
    if (test_writer_appends_tokens() != 0) {
        fprintf(stderr, "test_writer_appends_tokens failed\n");
        return 1;
    }
    if (test_writer_truncates_and_latches() != 0) {
        fprintf(stderr, "test_writer_truncates_and_latches failed\n");
        return 1;
    }
    if (test_writer_invalid_args() != 0) {
        fprintf(stderr, "test_writer_invalid_args failed\n");
        return 1;
    }
    if (test_accessor_children() != 0) {
        fprintf(stderr, "test_accessor_children failed\n");
        return 1;
    }
    if (test_accessor_sibling_order() != 0) {
        fprintf(stderr, "test_accessor_sibling_order failed\n");
        return 1;
    }
    if (test_span_valid() != 0) {
        fprintf(stderr, "test_span_valid failed\n");
        return 1;
    }
    if (test_span_eq_cstr() != 0) {
        fprintf(stderr, "test_span_eq_cstr failed\n");
        return 1;
    }
    if (test_parse_uint64_span() != 0) {
        fprintf(stderr, "test_parse_uint64_span failed\n");
        return 1;
    }
    if (test_writer_number_truncation() != 0) {
        fprintf(stderr, "test_writer_number_truncation failed\n");
        return 1;
    }
    return 0;
}
