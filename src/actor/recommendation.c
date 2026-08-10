#include "geistshell/recommendation.h"

#include <string.h>

enum field_id {
    FIELD_KIND = 0,
    FIELD_CAPABILITY,
    FIELD_COST,
    FIELD_USES_NETWORK,
    FIELD_CONFIDENCE_BP,
    FIELD_REASON,
    FIELD_COMMAND,
    FIELD_TARGET,
    FIELD_SLUG,
    FIELD_DESCRIPTION,
    FIELD_BODY,
    FIELD_COUNT,
    FIELD_UNKNOWN,
};

static void reject(struct spg_recommendation *out,
                   struct spg_recommendation_error *error,
                   const enum spg_recommendation_reject_reason reason,
                   const enum spg_status status, const uint32_t node_index,
                   const size_t offset) {
    if (out != nullptr) {
        *out = (struct spg_recommendation){
            .state         = SPG_RECOMMENDATION_REJECTED,
            .reject_reason = reason,
        };
    }
    if (error != nullptr) {
        *error = (struct spg_recommendation_error){
            .status        = status,
            .reject_reason = reason,
            .node_index    = node_index,
            .offset        = offset,
        };
    }
}

static bool has_third_child(const struct spg_sexpr_node nodes[static 1],
                            const uint32_t node) {
    const uint32_t second = spg_sexpr_second_child(nodes, node);
    return second != SPG_SEXPR_INVALID_INDEX &&
           nodes[second].next_sibling != SPG_SEXPR_INVALID_INDEX;
}

static enum field_id field_for_name(const size_t input_n, const char input[],
                                    const struct spg_text_span name) {
    if (spg_sexpr_span_eq_cstr(input_n, input, name, "kind")) {
        return FIELD_KIND;
    }
    if (spg_sexpr_span_eq_cstr(input_n, input, name, "capability")) {
        return FIELD_CAPABILITY;
    }
    if (spg_sexpr_span_eq_cstr(input_n, input, name, "cost")) {
        return FIELD_COST;
    }
    if (spg_sexpr_span_eq_cstr(input_n, input, name, "uses_network")) {
        return FIELD_USES_NETWORK;
    }
    if (spg_sexpr_span_eq_cstr(input_n, input, name, "confidence_bp")) {
        return FIELD_CONFIDENCE_BP;
    }
    if (spg_sexpr_span_eq_cstr(input_n, input, name, "reason")) {
        return FIELD_REASON;
    }
    if (spg_sexpr_span_eq_cstr(input_n, input, name, "command")) {
        return FIELD_COMMAND;
    }
    if (spg_sexpr_span_eq_cstr(input_n, input, name, "target")) {
        return FIELD_TARGET;
    }
    if (spg_sexpr_span_eq_cstr(input_n, input, name, "slug")) {
        return FIELD_SLUG;
    }
    if (spg_sexpr_span_eq_cstr(input_n, input, name, "description")) {
        return FIELD_DESCRIPTION;
    }
    if (spg_sexpr_span_eq_cstr(input_n, input, name, "body")) {
        return FIELD_BODY;
    }
    return FIELD_UNKNOWN;
}

static bool parse_bool_symbol(const size_t input_n, const char input[],
                              const struct spg_text_span span, bool *out) {
    if (spg_sexpr_span_eq_cstr(input_n, input, span, "true")) {
        *out = true;
        return true;
    }
    if (spg_sexpr_span_eq_cstr(input_n, input, span, "false")) {
        *out = false;
        return true;
    }
    return false;
}

static bool parse_action_kind(const size_t input_n, const char input[],
                              const struct spg_text_span span,
                              enum spg_action_kind *out) {
    if (spg_sexpr_span_eq_cstr(input_n, input, span, "simulator")) {
        *out = SPG_ACTION_SIMULATOR;
        return true;
    }
    if (spg_sexpr_span_eq_cstr(input_n, input, span, "local_shell")) {
        *out = SPG_ACTION_LOCAL_SHELL;
        return true;
    }
    if (spg_sexpr_span_eq_cstr(input_n, input, span, "ssh_auth_probe")) {
        *out = SPG_ACTION_SSH_AUTH_PROBE;
        return true;
    }
    if (spg_sexpr_span_eq_cstr(input_n, input, span, "memory_save")) {
        *out = SPG_ACTION_MEMORY_SAVE;
        return true;
    }
    if (spg_sexpr_span_eq_cstr(input_n, input, span, "memory_delete")) {
        *out = SPG_ACTION_MEMORY_DELETE;
        return true;
    }
    if (spg_sexpr_span_eq_cstr(input_n, input, span, "memory_read")) {
        *out = SPG_ACTION_MEMORY_READ;
        return true;
    }
    if (spg_sexpr_span_eq_cstr(input_n, input, span,
                               "machine_pause_process")) {
        *out = SPG_ACTION_MACHINE_PAUSE;
        return true;
    }
    if (spg_sexpr_span_eq_cstr(input_n, input, span,
                               "machine_resume_process")) {
        *out = SPG_ACTION_MACHINE_RESUME;
        return true;
    }
    if (spg_sexpr_span_eq_cstr(input_n, input, span, "machine_set_fan")) {
        *out = SPG_ACTION_MACHINE_FAN;
        return true;
    }
    if (spg_sexpr_span_eq_cstr(input_n, input, span, "finish")) {
        *out = SPG_ACTION_FINISH;
        return true;
    }
    return false;
}

static bool required_fields_seen(const bool seen[static FIELD_COUNT],
                                 const enum spg_action_kind kind) {
    /* finish is a control action with no capability/cost/network/confidence;
     * it needs only a kind and a reason. */
    if (kind == SPG_ACTION_FINISH) {
        return seen[FIELD_KIND] && seen[FIELD_REASON];
    }
    return seen[FIELD_KIND] && seen[FIELD_CAPABILITY] && seen[FIELD_COST] &&
           seen[FIELD_USES_NETWORK] && seen[FIELD_CONFIDENCE_BP] &&
           seen[FIELD_REASON];
}

static bool kind_fields_match(const struct spg_recommendation *out) {
    switch (out->action_kind) {
    case SPG_ACTION_SIMULATOR:
        return !out->action.uses_network && !out->has_command;
    case SPG_ACTION_LOCAL_SHELL:
        return !out->action.uses_network && out->has_command &&
               !out->has_target;
    case SPG_ACTION_SSH_AUTH_PROBE:
        return out->action.uses_network && !out->has_command &&
               out->has_target;
    case SPG_ACTION_MEMORY_SAVE:
        return !out->action.uses_network && !out->has_command &&
               !out->has_target && out->has_slug && out->has_description &&
               out->has_body;
    case SPG_ACTION_MEMORY_DELETE:
    case SPG_ACTION_MEMORY_READ:
        /* delete/read need only a slug. */
        return !out->action.uses_network && !out->has_command &&
               !out->has_target && out->has_slug && !out->has_description &&
               !out->has_body;
    case SPG_ACTION_MACHINE_PAUSE:
    case SPG_ACTION_MACHINE_RESUME:
        /* A machine action names a target and nothing else. No command field:
         * the whole point of a typed action is that the model cannot hand the
         * executor a string to run. */
        return !out->action.uses_network && !out->has_command &&
               out->has_target && !out->has_slug && !out->has_description &&
               !out->has_body;
    case SPG_ACTION_FINISH:
        /* finish carries no side-effect fields. */
        return !out->has_command && !out->has_target && !out->has_slug &&
               !out->has_description && !out->has_body;
    }
    return false;
}

const char *spg_recommendation_reject_reason_to_string(
    const enum spg_recommendation_reject_reason reason) {
    switch (reason) {
    case SPG_RECOMMENDATION_REJECT_NONE:
        return "SPG_RECOMMENDATION_REJECT_NONE";
    case SPG_RECOMMENDATION_REJECT_EMPTY:
        return "SPG_RECOMMENDATION_REJECT_EMPTY";
    case SPG_RECOMMENDATION_REJECT_SYNTAX:
        return "SPG_RECOMMENDATION_REJECT_SYNTAX";
    case SPG_RECOMMENDATION_REJECT_SCHEMA:
        return "SPG_RECOMMENDATION_REJECT_SCHEMA";
    case SPG_RECOMMENDATION_REJECT_UNKNOWN_KIND:
        return "SPG_RECOMMENDATION_REJECT_UNKNOWN_KIND";
    case SPG_RECOMMENDATION_REJECT_MISSING_FIELD:
        return "SPG_RECOMMENDATION_REJECT_MISSING_FIELD";
    case SPG_RECOMMENDATION_REJECT_DUPLICATE_FIELD:
        return "SPG_RECOMMENDATION_REJECT_DUPLICATE_FIELD";
    case SPG_RECOMMENDATION_REJECT_WRONG_VALUE:
        return "SPG_RECOMMENDATION_REJECT_WRONG_VALUE";
    case SPG_RECOMMENDATION_REJECT_KIND_MISMATCH:
        return "SPG_RECOMMENDATION_REJECT_KIND_MISMATCH";
    }
    return "SPG_RECOMMENDATION_REJECT_UNKNOWN";
}

enum spg_status spg_recommendation_parse(
    const size_t input_n, const char input[], const size_t token_capacity,
    struct spg_sexpr_token tokens[static token_capacity],
    const size_t node_capacity,
    struct spg_sexpr_node nodes[static node_capacity],
    struct spg_recommendation *out,
    struct spg_recommendation_error *error) {
    if (input == nullptr || tokens == nullptr || nodes == nullptr ||
        out == nullptr || token_capacity == 0u || node_capacity == 0u) {
        reject(out, error, SPG_RECOMMENDATION_REJECT_SCHEMA,
               SPG_E_INVALID_ARG, SPG_SEXPR_INVALID_INDEX, 0u);
        return SPG_E_INVALID_ARG;
    }
    reject(out, error, SPG_RECOMMENDATION_REJECT_EMPTY, SPG_OK,
           SPG_SEXPR_INVALID_INDEX, 0u);

    size_t                 token_count = 0u;
    size_t                 node_count  = 0u;
    struct spg_sexpr_error parse_error = {};
    enum spg_status status =
        spg_sexpr_parse_text(input_n, input, token_capacity, tokens,
                             node_capacity, nodes, &token_count, &node_count,
                             &parse_error);
    if (status == SPG_E_LIMIT) {
        reject(out, error, SPG_RECOMMENDATION_REJECT_SCHEMA, status,
               SPG_SEXPR_INVALID_INDEX, parse_error.offset);
        return status;
    }
    if (status != SPG_OK) {
        reject(out, error, SPG_RECOMMENDATION_REJECT_SYNTAX, status,
               SPG_SEXPR_INVALID_INDEX, parse_error.offset);
        return SPG_OK;
    }
    if (node_count == 0u) {
        reject(out, error, SPG_RECOMMENDATION_REJECT_EMPTY, SPG_OK,
               SPG_SEXPR_INVALID_INDEX, 0u);
        return SPG_OK;
    }
    if (node_count > UINT32_MAX || nodes[0].kind != SPG_SEXPR_NODE_LIST ||
        nodes[0].parent != SPG_SEXPR_INVALID_INDEX) {
        reject(out, error, SPG_RECOMMENDATION_REJECT_SCHEMA, SPG_E_SCHEMA, 0u,
               nodes[0].span.offset);
        return SPG_OK;
    }

    const uint32_t form_name = spg_sexpr_first_child(nodes, 0u);
    if (form_name == SPG_SEXPR_INVALID_INDEX ||
        nodes[form_name].kind != SPG_SEXPR_NODE_SYMBOL ||
        !spg_sexpr_span_eq_cstr(input_n, input, nodes[form_name].span, "recommend")) {
        reject(out, error, SPG_RECOMMENDATION_REJECT_SCHEMA, SPG_E_SCHEMA, 0u,
               nodes[0].span.offset);
        return SPG_OK;
    }

    bool seen[FIELD_COUNT] = {};
    struct spg_recommendation rec = {
        .state         = SPG_RECOMMENDATION_REJECTED,
        .reject_reason = SPG_RECOMMENDATION_REJECT_SCHEMA,
    };

    uint32_t field = nodes[form_name].next_sibling;
    while (field != SPG_SEXPR_INVALID_INDEX) {
        if (nodes[field].kind != SPG_SEXPR_NODE_LIST) {
            reject(out, error, SPG_RECOMMENDATION_REJECT_SCHEMA, SPG_E_SCHEMA,
                   field, nodes[field].span.offset);
            return SPG_OK;
        }
        const uint32_t name_node = spg_sexpr_first_child(nodes, field);
        const uint32_t value_node = spg_sexpr_second_child(nodes, field);
        if (name_node == SPG_SEXPR_INVALID_INDEX ||
            value_node == SPG_SEXPR_INVALID_INDEX || has_third_child(nodes, field) ||
            nodes[name_node].kind != SPG_SEXPR_NODE_SYMBOL) {
            reject(out, error, SPG_RECOMMENDATION_REJECT_SCHEMA, SPG_E_SCHEMA,
                   field, nodes[field].span.offset);
            return SPG_OK;
        }
        const enum field_id id =
            field_for_name(input_n, input, nodes[name_node].span);
        if (id == FIELD_UNKNOWN) {
            reject(out, error, SPG_RECOMMENDATION_REJECT_SCHEMA, SPG_E_SCHEMA,
                   name_node, nodes[name_node].span.offset);
            return SPG_OK;
        }
        if (seen[id]) {
            reject(out, error, SPG_RECOMMENDATION_REJECT_DUPLICATE_FIELD,
                   SPG_E_SCHEMA, name_node, nodes[name_node].span.offset);
            return SPG_OK;
        }
        seen[id] = true;

        const struct spg_sexpr_node *value = &nodes[value_node];
        switch (id) {
        case FIELD_KIND:
            if (value->kind != SPG_SEXPR_NODE_SYMBOL) {
                reject(out, error, SPG_RECOMMENDATION_REJECT_WRONG_VALUE,
                       SPG_E_SCHEMA, value_node, value->span.offset);
                return SPG_OK;
            }
            rec.kind = value->span;
            if (!parse_action_kind(input_n, input, value->span,
                                   &rec.action_kind)) {
                reject(out, error, SPG_RECOMMENDATION_REJECT_UNKNOWN_KIND,
                       SPG_E_SCHEMA, value_node, value->span.offset);
                return SPG_OK;
            }
            break;
        case FIELD_CAPABILITY:
            if (!spg_sexpr_string_payload_span(value, &rec.capability)) {
                reject(out, error, SPG_RECOMMENDATION_REJECT_WRONG_VALUE,
                       SPG_E_SCHEMA, value_node, value->span.offset);
                return SPG_OK;
            }
            break;
        case FIELD_COST:
            if (value->kind != SPG_SEXPR_NODE_SYMBOL ||
                spg_sexpr_parse_uint64_span(input_n, input, value->span,
                                  &rec.action.cost) != SPG_OK ||
                rec.action.cost == 0u) {
                reject(out, error, SPG_RECOMMENDATION_REJECT_WRONG_VALUE,
                       SPG_E_SCHEMA, value_node, value->span.offset);
                return SPG_OK;
            }
            break;
        case FIELD_USES_NETWORK:
            if (value->kind != SPG_SEXPR_NODE_SYMBOL ||
                !parse_bool_symbol(input_n, input, value->span,
                                   &rec.action.uses_network)) {
                reject(out, error, SPG_RECOMMENDATION_REJECT_WRONG_VALUE,
                       SPG_E_SCHEMA, value_node, value->span.offset);
                return SPG_OK;
            }
            break;
        case FIELD_CONFIDENCE_BP:
            if (value->kind != SPG_SEXPR_NODE_SYMBOL ||
                spg_sexpr_parse_uint64_span(input_n, input, value->span,
                                  &rec.confidence_bp) != SPG_OK ||
                rec.confidence_bp > 10000u) {
                reject(out, error, SPG_RECOMMENDATION_REJECT_WRONG_VALUE,
                       SPG_E_SCHEMA, value_node, value->span.offset);
                return SPG_OK;
            }
            break;
        case FIELD_REASON:
            if (!spg_sexpr_string_payload_span(value, &rec.reason)) {
                reject(out, error, SPG_RECOMMENDATION_REJECT_WRONG_VALUE,
                       SPG_E_SCHEMA, value_node, value->span.offset);
                return SPG_OK;
            }
            break;
        case FIELD_COMMAND:
            if (!spg_sexpr_string_payload_span(value, &rec.command)) {
                reject(out, error, SPG_RECOMMENDATION_REJECT_WRONG_VALUE,
                       SPG_E_SCHEMA, value_node, value->span.offset);
                return SPG_OK;
            }
            rec.has_command = true;
            break;
        case FIELD_TARGET:
            if (!spg_sexpr_string_payload_span(value, &rec.target)) {
                reject(out, error, SPG_RECOMMENDATION_REJECT_WRONG_VALUE,
                       SPG_E_SCHEMA, value_node, value->span.offset);
                return SPG_OK;
            }
            rec.has_target = true;
            break;
        case FIELD_SLUG:
            if (!spg_sexpr_string_payload_span(value, &rec.mem_slug)) {
                reject(out, error, SPG_RECOMMENDATION_REJECT_WRONG_VALUE,
                       SPG_E_SCHEMA, value_node, value->span.offset);
                return SPG_OK;
            }
            rec.has_slug = true;
            break;
        case FIELD_DESCRIPTION:
            if (!spg_sexpr_string_payload_span(value, &rec.mem_description)) {
                reject(out, error, SPG_RECOMMENDATION_REJECT_WRONG_VALUE,
                       SPG_E_SCHEMA, value_node, value->span.offset);
                return SPG_OK;
            }
            rec.has_description = true;
            break;
        case FIELD_BODY:
            if (!spg_sexpr_string_payload_span(value, &rec.mem_body)) {
                reject(out, error, SPG_RECOMMENDATION_REJECT_WRONG_VALUE,
                       SPG_E_SCHEMA, value_node, value->span.offset);
                return SPG_OK;
            }
            rec.has_body = true;
            break;
        case FIELD_COUNT:
        case FIELD_UNKNOWN:
            reject(out, error, SPG_RECOMMENDATION_REJECT_SCHEMA, SPG_E_SCHEMA,
                   field, nodes[field].span.offset);
            return SPG_OK;
        }

        field = nodes[field].next_sibling;
    }

    if (!required_fields_seen(seen, rec.action_kind)) {
        reject(out, error, SPG_RECOMMENDATION_REJECT_MISSING_FIELD,
               SPG_E_SCHEMA, 0u, nodes[0].span.offset);
        return SPG_OK;
    }
    rec.action.kind       = rec.action_kind;
    rec.action.capability = rec.capability;
    if (!kind_fields_match(&rec)) {
        reject(out, error, SPG_RECOMMENDATION_REJECT_KIND_MISMATCH,
               SPG_E_SCHEMA, 0u, nodes[0].span.offset);
        return SPG_OK;
    }

    rec.state         = SPG_RECOMMENDATION_VALID;
    rec.reject_reason = SPG_RECOMMENDATION_REJECT_NONE;
    *out              = rec;
    if (error != nullptr) {
        *error = (struct spg_recommendation_error){
            .status        = SPG_OK,
            .reject_reason = SPG_RECOMMENDATION_REJECT_NONE,
            .node_index    = 0u,
            .offset        = nodes[0].span.offset,
        };
    }
    return SPG_OK;
}
