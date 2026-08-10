/* Per-model driving instructions, read from a file so a benchmark number can
 * be traced back to the profile that produced it. */

#include "geistshell/model_profile.h"

#include <string.h>

static uint32_t field_value(const size_t input_n, const char input[],
                            const struct spg_sexpr_node nodes[static 1],
                            const uint32_t parent, const char *name) {
    uint32_t field = spg_sexpr_first_child(nodes, parent);
    while (field != SPG_SEXPR_INVALID_INDEX) {
        if (nodes[field].kind == SPG_SEXPR_NODE_LIST) {
            const uint32_t head = spg_sexpr_first_child(nodes, field);
            if (head != SPG_SEXPR_INVALID_INDEX &&
                spg_sexpr_span_eq_cstr(input_n, input, nodes[head].span,
                                       name)) {
                return spg_sexpr_second_child(nodes, field);
            }
        }
        field = nodes[field].next_sibling;
    }
    return SPG_SEXPR_INVALID_INDEX;
}

static bool copy_text(const size_t input_n, const char input[],
                      const struct spg_sexpr_node *node, const size_t cap,
                      char dst[]) {
    struct spg_text_span span = {};
    if (node == nullptr) {
        return false;
    }
    if (!spg_sexpr_string_payload_span(node, &span)) {
        span = node->span; /* a bare symbol is acceptable too */
    }
    if (!spg_sexpr_span_valid(input_n, span) || span.length + 1u > cap) {
        return false;
    }
    memcpy(dst, input + span.offset, span.length);
    dst[span.length] = '\0';
    return true;
}

static enum spg_status parse_template(const size_t input_n, const char input[],
                                      const struct spg_text_span span,
                                      enum spg_chat_template     *out) {
    const struct {
        const char            *name;
        enum spg_chat_template value;
    } names[] = {
        {"auto", SPG_TEMPLATE_AUTO},     {"none", SPG_TEMPLATE_NONE},
        {"gemma", SPG_TEMPLATE_GEMMA},   {"llama3", SPG_TEMPLATE_LLAMA3},
        {"generic", SPG_TEMPLATE_GENERIC},
    };
    for (size_t i = 0u; i < sizeof names / sizeof names[0]; i += 1u) {
        if (spg_sexpr_span_eq_cstr(input_n, input, span, names[i].name)) {
            *out = names[i].value;
            return SPG_OK;
        }
    }
    /* An unrecognised template must not fall back to `none`: the run would
     * then silently be the very experiment this profile exists to replace. */
    return SPG_E_SCHEMA;
}

static enum spg_status parse_bool(const size_t input_n, const char input[],
                                  const struct spg_text_span span, bool *out) {
    if (spg_sexpr_span_eq_cstr(input_n, input, span, "true")) {
        *out = true;
        return SPG_OK;
    }
    if (spg_sexpr_span_eq_cstr(input_n, input, span, "false")) {
        *out = false;
        return SPG_OK;
    }
    return SPG_E_SCHEMA;
}

enum spg_status spg_model_profile_load(
    const size_t input_n, const char input[], const size_t token_capacity,
    struct spg_sexpr_token tokens[static token_capacity],
    const size_t           node_capacity,
    struct spg_sexpr_node  nodes[static node_capacity],
    struct spg_model_profile *out) {
    if (out == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out = (struct spg_model_profile){};

    size_t                 token_count = 0u;
    size_t                 node_count  = 0u;
    struct spg_sexpr_error error       = {};
    const enum spg_status  status      = spg_sexpr_parse_text(
        input_n, input, token_capacity, tokens, node_capacity, nodes,
        &token_count, &node_count, &error);
    if (status != SPG_OK) {
        return status;
    }
    if (node_count == 0u) {
        return SPG_E_FORMAT;
    }
    const uint32_t root = 0u;
    const uint32_t head = spg_sexpr_first_child(nodes, root);
    if (nodes[root].kind != SPG_SEXPR_NODE_LIST ||
        head == SPG_SEXPR_INVALID_INDEX ||
        !spg_sexpr_span_eq_cstr(input_n, input, nodes[head].span,
                                "model_profile")) {
        return SPG_E_SCHEMA;
    }

    uint32_t node = field_value(input_n, input, nodes, root, "name");
    if (node != SPG_SEXPR_INVALID_INDEX &&
        !copy_text(input_n, input, &nodes[node], SPG_PROFILE_NAME_CAP,
                   out->name)) {
        return SPG_E_SCHEMA;
    }
    node = field_value(input_n, input, nodes, root, "arch");
    if (node != SPG_SEXPR_INVALID_INDEX &&
        !copy_text(input_n, input, &nodes[node], SPG_PROFILE_NAME_CAP,
                   out->arch)) {
        return SPG_E_SCHEMA;
    }
    node = field_value(input_n, input, nodes, root, "template");
    if (node != SPG_SEXPR_INVALID_INDEX) {
        const enum spg_status ts =
            parse_template(input_n, input, nodes[node].span, &out->chat_template);
        if (ts != SPG_OK) {
            return ts;
        }
    }
    node = field_value(input_n, input, nodes, root, "constrained");
    if (node != SPG_SEXPR_INVALID_INDEX) {
        const enum spg_status bs =
            parse_bool(input_n, input, nodes[node].span, &out->constrained);
        if (bs != SPG_OK) {
            return bs;
        }
        out->has_constrained = true;
    }
    node = field_value(input_n, input, nodes, root, "temperature");
    if (node != SPG_SEXPR_INVALID_INDEX) {
        if (spg_sexpr_parse_uint64_span(input_n, input, nodes[node].span,
                                        &out->temperature_bp) != SPG_OK) {
            return SPG_E_SCHEMA;
        }
        out->has_temperature = true;
    }
    node = field_value(input_n, input, nodes, root, "best_of");
    if (node != SPG_SEXPR_INVALID_INDEX) {
        if (spg_sexpr_parse_uint64_span(input_n, input, nodes[node].span,
                                        &out->best_of) != SPG_OK ||
            out->best_of == 0u) {
            return SPG_E_SCHEMA;
        }
        out->has_best_of = true;
    }
    out->present = true;
    return SPG_OK;
}

enum spg_chat_template spg_template_for_arch(const char *arch) {
    if (arch == nullptr || arch[0] == '\0') {
        return SPG_TEMPLATE_NONE;
    }
    if (strstr(arch, "gemma") != nullptr) {
        return SPG_TEMPLATE_GEMMA;
    }
    if (strstr(arch, "llama") != nullptr) {
        return SPG_TEMPLATE_LLAMA3;
    }
    /* BitNet b1.58 is a base model with no chat format of its own. Sending
     * llama3 markers to it would put unfamiliar tokens in the prompt; the
     * profile can still ask for a template explicitly, which is the point of
     * having the file. */
    return SPG_TEMPLATE_NONE;
}

/* --- framing ------------------------------------------------------------ */

struct frame {
    size_t capacity;
    char  *dst;
    size_t used;
    bool   overflowed;
};

static void put(struct frame *f, const char *text) {
    for (size_t i = 0u; text != nullptr && text[i] != '\0'; i += 1u) {
        if (f->used + 1u < f->capacity) {
            f->dst[f->used] = text[i];
        } else {
            f->overflowed = true;
        }
        f->used += 1u;
    }
}

static void put_n(struct frame *f, const size_t n, const char text[]) {
    for (size_t i = 0u; i < n; i += 1u) {
        if (f->used + 1u < f->capacity) {
            f->dst[f->used] = text[i];
        } else {
            f->overflowed = true;
        }
        f->used += 1u;
    }
}

enum spg_status spg_chat_frame(const enum spg_chat_template tmpl,
                               const char *system, const size_t user_n,
                               const char user[], const size_t dst_capacity,
                               char dst[static dst_capacity],
                               size_t *out_used) {
    if (out_used == nullptr || dst_capacity == 0u ||
        (user_n > 0u && user == nullptr)) {
        return SPG_E_INVALID_ARG;
    }
    struct frame f = {.capacity = dst_capacity, .dst = dst};

    switch (tmpl) {
    case SPG_TEMPLATE_AUTO:
    case SPG_TEMPLATE_NONE:
        /* Unchanged behaviour, byte for byte: a base model gets the prompt it
         * always got. */
        put(&f, system);
        put_n(&f, user_n, user);
        break;
    case SPG_TEMPLATE_GEMMA:
        put(&f, "<start_of_turn>user\n");
        put(&f, system);
        put_n(&f, user_n, user);
        put(&f, "<end_of_turn>\n<start_of_turn>model\n");
        break;
    case SPG_TEMPLATE_LLAMA3:
        if (system != nullptr && system[0] != '\0') {
            put(&f, "<|start_header_id|>system<|end_header_id|>\n\n");
            put(&f, system);
            put(&f, "<|eot_id|>");
        }
        put(&f, "<|start_header_id|>user<|end_header_id|>\n\n");
        put_n(&f, user_n, user);
        put(&f, "<|eot_id|><|start_header_id|>assistant<|end_header_id|>\n\n");
        break;
    case SPG_TEMPLATE_GENERIC:
        /* No special tokens: plain labels a base model can read as text. The
         * point is a visible boundary between instruction and answer, not a
         * format it was trained on. */
        put(&f, system);
        put(&f, "### Input\n");
        put_n(&f, user_n, user);
        put(&f, "\n### Response\n");
        break;
    }

    *out_used = f.used;
    if (f.overflowed) {
        dst[0] = '\0'; /* half a template is worse than none */
        return SPG_E_LIMIT;
    }
    dst[f.used] = '\0';
    return SPG_OK;
}

const char *spg_chat_template_to_string(const enum spg_chat_template tmpl) {
    switch (tmpl) {
    case SPG_TEMPLATE_AUTO:
        return "auto";
    case SPG_TEMPLATE_NONE:
        return "none";
    case SPG_TEMPLATE_GEMMA:
        return "gemma";
    case SPG_TEMPLATE_LLAMA3:
        return "llama3";
    case SPG_TEMPLATE_GENERIC:
        return "generic";
    }
    return "none";
}
