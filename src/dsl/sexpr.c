#include "geistshell/sexpr.h"

#include <stdbool.h>
#include <string.h>

static void set_error(struct spg_sexpr_error *error,
                      const enum spg_status   status,
                      const size_t            offset,
                      const size_t            line,
                      const size_t            column) {
    if (error == nullptr) {
        return;
    }
    error->status = status;
    error->offset = offset;
    error->line   = line;
    error->column = column;
}

bool spg_sexpr_span_valid(const size_t               input_n,
                          const struct spg_text_span span) {
    return span.offset <= input_n && span.length <= input_n - span.offset;
}

bool spg_sexpr_span_eq_cstr(const size_t input_n, const char input[],
                            const struct spg_text_span span,
                            const char                *text) {
    if (input == nullptr || text == nullptr ||
        !spg_sexpr_span_valid(input_n, span)) {
        return false;
    }
    const size_t text_n = strlen(text);
    return span.length == text_n &&
           memcmp(input + span.offset, text, text_n) == 0;
}

uint32_t spg_sexpr_first_child(const struct spg_sexpr_node nodes[static 1],
                               const uint32_t              node) {
    return nodes[node].first_child;
}

uint32_t spg_sexpr_second_child(const struct spg_sexpr_node nodes[static 1],
                                const uint32_t              node) {
    const uint32_t first = nodes[node].first_child;
    if (first == SPG_SEXPR_INVALID_INDEX) {
        return SPG_SEXPR_INVALID_INDEX;
    }
    return nodes[first].next_sibling;
}

bool spg_sexpr_string_payload_span(const struct spg_sexpr_node *node,
                                   struct spg_text_span        *out) {
    if (node == nullptr || out == nullptr ||
        node->kind != SPG_SEXPR_NODE_STRING || node->span.length < 2u) {
        return false;
    }
    *out = (struct spg_text_span){
        .offset = node->span.offset + 1u,
        .length = node->span.length - 2u,
    };
    return true;
}

enum spg_status spg_sexpr_parse_uint64_span(const size_t input_n,
                                            const char   input[],
                                            const struct spg_text_span span,
                                            uint64_t                  *out) {
    if (out == nullptr || input == nullptr || span.length == 0u ||
        !spg_sexpr_span_valid(input_n, span)) {
        return SPG_E_INVALID_ARG;
    }
    uint64_t value = 0u;
    for (size_t i = 0u; i < span.length; i += 1u) {
        const char c = input[span.offset + i];
        if (c < '0' || c > '9') {
            return SPG_E_FORMAT;
        }
        const uint64_t digit = (uint64_t)(c - '0');
        if (value > (UINT64_MAX - digit) / 10u) {
            return SPG_E_OVERFLOW;
        }
        value = (value * 10u) + digit;
    }
    *out = value;
    return SPG_OK;
}

static bool is_space(const char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static bool is_delim(const char c) {
    return is_space(c) || c == '(' || c == ')' || c == ';';
}

static bool valid_symbol_char(const char c) {
    const unsigned char u = (unsigned char)c;
    return u >= 33u && u <= 126u && c != '"' && c != '(' && c != ')' &&
           c != ';' && c != '\\';
}

static enum spg_status push_token(
    const enum spg_sexpr_token_kind kind, const size_t offset,
    const size_t length, const size_t line, const size_t column,
    size_t token_capacity,
    struct spg_sexpr_token tokens[static token_capacity], size_t *count,
    struct spg_sexpr_error *error) {
    if (*count >= token_capacity) {
        set_error(error, SPG_E_LIMIT, offset, line, column);
        return SPG_E_LIMIT;
    }
    tokens[*count] = (struct spg_sexpr_token){
        .kind   = kind,
        .span   = {.offset = offset, .length = length},
        .line   = line,
        .column = column,
    };
    *count += 1u;
    return SPG_OK;
}

enum spg_status
spg_sexpr_tokenize(const size_t input_n,
                   const char   input[],
                   const size_t token_capacity,
                   struct spg_sexpr_token tokens[static token_capacity],
                   size_t *token_count, struct spg_sexpr_error *error) {
    if (input == nullptr || tokens == nullptr || token_count == nullptr ||
        token_capacity == 0u) {
        set_error(error, SPG_E_INVALID_ARG, 0u, 1u, 1u);
        return SPG_E_INVALID_ARG;
    }

    *token_count = 0u;
    set_error(error, SPG_OK, 0u, 1u, 1u);

    size_t i      = 0u;
    size_t line   = 1u;
    size_t column = 1u;
    while (i < input_n) {
        const char c = input[i];
        if (c == '\0') {
            set_error(error, SPG_E_FORMAT, i, line, column);
            return SPG_E_FORMAT;
        }
        if (c == '\n') {
            i += 1u;
            line += 1u;
            column = 1u;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') {
            i += 1u;
            column += 1u;
            continue;
        }
        if (c == ';') {
            while (i < input_n && input[i] != '\n') {
                i += 1u;
                column += 1u;
            }
            continue;
        }
        if (c == '(' || c == ')') {
            const enum spg_sexpr_token_kind kind =
                c == '(' ? SPG_SEXPR_TOKEN_LPAREN : SPG_SEXPR_TOKEN_RPAREN;
            const enum spg_status status =
                push_token(kind, i, 1u, line, column, token_capacity, tokens,
                           token_count, error);
            if (status != SPG_OK) {
                return status;
            }
            i += 1u;
            column += 1u;
            continue;
        }
        if (c == '"') {
            const size_t start        = i;
            const size_t start_line   = line;
            const size_t start_column = column;
            i += 1u;
            column += 1u;

            bool closed = false;
            while (i < input_n) {
                const char sc = input[i];
                if (sc == '\0' || sc == '\n' || sc == '\r') {
                    set_error(error, SPG_E_FORMAT, i, line, column);
                    return SPG_E_FORMAT;
                }
                if (sc == '\\') {
                    if (i + 1u >= input_n) {
                        set_error(error, SPG_E_FORMAT, i, line, column);
                        return SPG_E_FORMAT;
                    }
                    const char ec = input[i + 1u];
                    if (!(ec == '"' || ec == '\\' || ec == 'n' || ec == 't' ||
                          ec == 'r')) {
                        set_error(error, SPG_E_FORMAT, i, line, column);
                        return SPG_E_FORMAT;
                    }
                    i += 2u;
                    column += 2u;
                    continue;
                }
                if (sc == '"') {
                    i += 1u;
                    column += 1u;
                    closed = true;
                    break;
                }
                i += 1u;
                column += 1u;
            }
            if (!closed) {
                set_error(error, SPG_E_FORMAT, start, start_line,
                          start_column);
                return SPG_E_FORMAT;
            }
            const enum spg_status status =
                push_token(SPG_SEXPR_TOKEN_STRING, start, i - start,
                           start_line, start_column, token_capacity, tokens,
                           token_count, error);
            if (status != SPG_OK) {
                return status;
            }
            continue;
        }

        if (!valid_symbol_char(c)) {
            set_error(error, SPG_E_FORMAT, i, line, column);
            return SPG_E_FORMAT;
        }
        const size_t start        = i;
        const size_t start_line   = line;
        const size_t start_column = column;
        while (i < input_n && !is_delim(input[i])) {
            if (!valid_symbol_char(input[i])) {
                set_error(error, SPG_E_FORMAT, i, line, column);
                return SPG_E_FORMAT;
            }
            i += 1u;
            column += 1u;
        }
        const enum spg_status status =
            push_token(SPG_SEXPR_TOKEN_SYMBOL, start, i - start, start_line,
                       start_column, token_capacity, tokens, token_count,
                       error);
        if (status != SPG_OK) {
            return status;
        }
    }
    return SPG_OK;
}

static void append_child(const uint32_t parent, const uint32_t child,
                         struct spg_sexpr_node nodes[static 1]) {
    if (nodes[parent].first_child == SPG_SEXPR_INVALID_INDEX) {
        nodes[parent].first_child = child;
    } else {
        nodes[nodes[parent].last_child].next_sibling = child;
    }
    nodes[parent].last_child = child;
}

static enum spg_status push_node(
    const enum spg_sexpr_node_kind kind, const struct spg_text_span span,
    const uint32_t parent, size_t node_capacity,
    struct spg_sexpr_node nodes[static node_capacity], size_t *node_count,
    uint32_t *out_index) {
    if (*node_count >= node_capacity || *node_count > UINT32_MAX) {
        return SPG_E_LIMIT;
    }
    const uint32_t index = (uint32_t)*node_count;
    nodes[index]         = (struct spg_sexpr_node){
                .kind         = kind,
                .span         = span,
                .parent       = parent,
                .first_child  = SPG_SEXPR_INVALID_INDEX,
                .next_sibling = SPG_SEXPR_INVALID_INDEX,
                .last_child   = SPG_SEXPR_INVALID_INDEX,
    };
    *node_count += 1u;
    *out_index = index;
    if (parent != SPG_SEXPR_INVALID_INDEX) {
        append_child(parent, index, nodes);
    }
    return SPG_OK;
}

enum spg_status
spg_sexpr_parse(const size_t token_n,
                const struct spg_sexpr_token tokens[],
                const size_t node_capacity,
                struct spg_sexpr_node nodes[static node_capacity],
                size_t *node_count, struct spg_sexpr_error *error) {
    if (tokens == nullptr || nodes == nullptr || node_count == nullptr ||
        node_capacity == 0u) {
        set_error(error, SPG_E_INVALID_ARG, 0u, 1u, 1u);
        return SPG_E_INVALID_ARG;
    }

    *node_count = 0u;
    set_error(error, SPG_OK, 0u, 1u, 1u);

    if (token_n == 0u) {
        return SPG_OK;
    }

    uint32_t current_parent = SPG_SEXPR_INVALID_INDEX;
    for (size_t i = 0u; i < token_n; i += 1u) {
        const struct spg_sexpr_token token = tokens[i];
        if (token.kind == SPG_SEXPR_TOKEN_LPAREN) {
            uint32_t index = SPG_SEXPR_INVALID_INDEX;
            const enum spg_status status =
                push_node(SPG_SEXPR_NODE_LIST, token.span, current_parent,
                          node_capacity, nodes, node_count, &index);
            if (status != SPG_OK) {
                set_error(error, status, token.span.offset, token.line,
                          token.column);
                *node_count = 0u;
                return status;
            }
            current_parent = index;
            continue;
        }
        if (token.kind == SPG_SEXPR_TOKEN_RPAREN) {
            if (current_parent == SPG_SEXPR_INVALID_INDEX) {
                set_error(error, SPG_E_FORMAT, token.span.offset, token.line,
                          token.column);
                *node_count = 0u;
                return SPG_E_FORMAT;
            }
            nodes[current_parent].span.length =
                (token.span.offset + token.span.length) -
                nodes[current_parent].span.offset;
            current_parent = nodes[current_parent].parent;
            continue;
        }

        const enum spg_sexpr_node_kind kind =
            token.kind == SPG_SEXPR_TOKEN_SYMBOL ? SPG_SEXPR_NODE_SYMBOL
                                                 : SPG_SEXPR_NODE_STRING;
        uint32_t index = SPG_SEXPR_INVALID_INDEX;
        const enum spg_status status =
            push_node(kind, token.span, current_parent, node_capacity, nodes,
                      node_count, &index);
        if (status != SPG_OK) {
            set_error(error, status, token.span.offset, token.line,
                      token.column);
            *node_count = 0u;
            return status;
        }
    }

    if (current_parent != SPG_SEXPR_INVALID_INDEX) {
        const struct spg_sexpr_node open = nodes[current_parent];
        set_error(error, SPG_E_FORMAT, open.span.offset, 1u, 1u);
        *node_count = 0u;
        return SPG_E_FORMAT;
    }

    return SPG_OK;
}

enum spg_status spg_sexpr_parse_text(
    const size_t input_n, const char input[],
    const size_t token_capacity,
    struct spg_sexpr_token tokens[static token_capacity],
    const size_t node_capacity,
    struct spg_sexpr_node nodes[static node_capacity], size_t *token_count,
    size_t *node_count, struct spg_sexpr_error *error) {
    if (token_count == nullptr || node_count == nullptr) {
        set_error(error, SPG_E_INVALID_ARG, 0u, 1u, 1u);
        return SPG_E_INVALID_ARG;
    }
    *token_count = 0u;
    *node_count  = 0u;

    enum spg_status status =
        spg_sexpr_tokenize(input_n, input, token_capacity, tokens, token_count,
                           error);
    if (status != SPG_OK) {
        return status;
    }
    status = spg_sexpr_parse(*token_count, tokens, node_capacity, nodes,
                             node_count, error);
    if (status != SPG_OK) {
        return status;
    }
    return SPG_OK;
}

void spg_sexpr_writer_init(struct spg_sexpr_writer *writer,
                           const size_t capacity,
                           char buffer[static capacity]) {
    if (writer == nullptr) {
        return;
    }
    writer->buffer    = buffer;
    writer->capacity  = capacity;
    writer->used      = 0u;
    writer->truncated = false;
    if (buffer != nullptr && capacity > 0u) {
        buffer[0] = '\0';
    }
}

enum spg_status spg_sexpr_writer_append_text(struct spg_sexpr_writer *writer,
                                             const char *text) {
    if (writer == nullptr || writer->buffer == nullptr || text == nullptr ||
        writer->used >= writer->capacity) {
        if (writer != nullptr) {
            writer->truncated = true;
        }
        return SPG_E_INVALID_ARG;
    }
    if (writer->truncated) {
        return SPG_E_LIMIT;
    }
    const size_t n = strlen(text);
    if (n > writer->capacity - writer->used - 1u) {
        writer->truncated = true;
        return SPG_E_LIMIT;
    }
    memcpy(writer->buffer + writer->used, text, n);
    writer->used += n;
    writer->buffer[writer->used] = '\0';
    return SPG_OK;
}

/* Decimal-format an unsigned into a NUL-terminated buffer without snprintf's
 * format parsing: fill back-to-front and return the pointer to the first digit.
 * The buffer is sized for the widest unsigned (3 digits per byte + NUL). */
static char *u64_to_dec(uint64_t value, char buffer[static 21]) {
    size_t i  = 21u;
    buffer[--i] = '\0';
    do {
        buffer[--i] = (char)('0' + (unsigned)(value % 10u));
        value /= 10u;
    } while (value != 0u);
    return buffer + i;
}

enum spg_status spg_sexpr_writer_append_u64(struct spg_sexpr_writer *writer,
                                            const uint64_t value) {
    char buffer[21];
    return spg_sexpr_writer_append_text(writer, u64_to_dec(value, buffer));
}

enum spg_status spg_sexpr_writer_append_size(struct spg_sexpr_writer *writer,
                                             const size_t value) {
    char buffer[21];
    return spg_sexpr_writer_append_text(writer, u64_to_dec((uint64_t)value,
                                                           buffer));
}
