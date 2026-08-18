#include "geistshell/policy_config.h"

#include "geistshell/schema.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static const struct spg_schema_field_rule policy_fields[] = {
    {.name       = "network_default",
     .value_kind = SPG_SCHEMA_VALUE_SYMBOL,
     .min_values = 1u,
     .max_values = 1u,
     .required   = true,
     .unique     = true},
    {.name       = "budgets",
     .value_kind = SPG_SCHEMA_VALUE_LIST,
     .min_values = 5u,
     .max_values = 7u, /* 5 required + optional memory/machine_actions */
     .required   = true,
     .unique     = true},
    {.name       = "capability",
     .value_kind = SPG_SCHEMA_VALUE_ANY,
     .min_values = 1u,
     .max_values = SPG_POLICY_MAX_CAPABILITIES,
     .required   = false,
     .unique     = true},
};

static const struct spg_schema_form_rule policy_forms[] = {
    {.name                 = "policy",
     .field_rule_count     = sizeof(policy_fields) / sizeof(policy_fields[0]),
     .field_rules          = policy_fields,
     .allow_unknown_fields = false,
     .min_fields           = 2u,
     .max_fields           = 3u},
};

static const struct spg_schema policy_schema = {
    .form_rule_count               = sizeof(policy_forms) / sizeof(policy_forms[0]),
    .form_rules                    = policy_forms,
    .allow_unknown_top_level_forms = false,
    .max_top_level_forms           = 1u,
    .max_depth                     = 5u,
};

static void set_error(struct spg_policy_config_error *error,
                      const enum spg_status           status,
                      const uint32_t                  node_index,
                      const size_t                    offset) {
    if (error == nullptr) {
        return;
    }
    error->status     = status;
    error->node_index = node_index;
    error->offset     = offset;
}

static uint32_t find_field(const size_t input_n, const char input[],
                           const struct spg_sexpr_node nodes[static 1],
                           const uint32_t policy_node, const char *name) {
    const uint32_t form_name = spg_sexpr_first_child(nodes, policy_node);
    if (form_name == SPG_SEXPR_INVALID_INDEX) {
        return SPG_SEXPR_INVALID_INDEX;
    }
    uint32_t field = nodes[form_name].next_sibling;
    while (field != SPG_SEXPR_INVALID_INDEX) {
        const uint32_t field_name = spg_sexpr_first_child(nodes, field);
        if (field_name != SPG_SEXPR_INVALID_INDEX &&
            nodes[field_name].kind == SPG_SEXPR_NODE_SYMBOL &&
            spg_sexpr_span_eq_cstr(input_n, input, nodes[field_name].span, name)) {
            return field;
        }
        field = nodes[field].next_sibling;
    }
    return SPG_SEXPR_INVALID_INDEX;
}

static enum spg_status parse_budgets(
    const size_t input_n, const char input[],
    const struct spg_sexpr_node nodes[static 1], const uint32_t policy_node,
    struct spg_run_budgets *budgets, struct spg_policy_config_error *error) {
    const uint32_t field =
        find_field(input_n, input, nodes, policy_node, "budgets");
    if (field == SPG_SEXPR_INVALID_INDEX) {
        set_error(error, SPG_E_SCHEMA, policy_node, nodes[policy_node].span.offset);
        return SPG_E_SCHEMA;
    }

    uint32_t        err_node   = SPG_SEXPR_INVALID_INDEX;
    size_t          err_offset = 0u;
    const enum spg_status status = spg_run_budgets_parse(
        input_n, input, nodes, field, budgets, &err_node, &err_offset);
    if (status != SPG_OK) {
        set_error(error, status, err_node, err_offset);
    }
    return status;
}

static enum spg_status parse_network_default(
    const size_t input_n, const char input[],
    const struct spg_sexpr_node nodes[static 1], const uint32_t policy_node,
    enum spg_policy_network_default *out,
    struct spg_policy_config_error *error) {
    const uint32_t field =
        find_field(input_n, input, nodes, policy_node, "network_default");
    const uint32_t value = spg_sexpr_second_child(nodes, field);
    if (field == SPG_SEXPR_INVALID_INDEX ||
        value == SPG_SEXPR_INVALID_INDEX ||
        nodes[value].kind != SPG_SEXPR_NODE_SYMBOL) {
        set_error(error, SPG_E_SCHEMA, policy_node, nodes[policy_node].span.offset);
        return SPG_E_SCHEMA;
    }
    if (spg_sexpr_span_eq_cstr(input_n, input, nodes[value].span, "deny")) {
        *out = SPG_POLICY_NETWORK_DENY;
        return SPG_OK;
    }
    if (spg_sexpr_span_eq_cstr(input_n, input, nodes[value].span, "allow")) {
        *out = SPG_POLICY_NETWORK_ALLOW;
        return SPG_OK;
    }
    set_error(error, SPG_E_SCHEMA, value, nodes[value].span.offset);
    return SPG_E_SCHEMA;
}

static enum spg_status parse_cap_kind(
    const size_t input_n, const char input[], const struct spg_text_span span,
    enum spg_policy_capability_kind *out) {
    if (spg_sexpr_span_eq_cstr(input_n, input, span, "local_shell")) {
        *out = SPG_POLICY_CAP_LOCAL_SHELL;
        return SPG_OK;
    }
    if (spg_sexpr_span_eq_cstr(input_n, input, span, "ssh_auth_probe")) {
        *out = SPG_POLICY_CAP_SSH_AUTH_PROBE;
        return SPG_OK;
    }
    if (spg_sexpr_span_eq_cstr(input_n, input, span, "simulator")) {
        *out = SPG_POLICY_CAP_SIMULATOR;
        return SPG_OK;
    }
    if (spg_sexpr_span_eq_cstr(input_n, input, span, "memory")) {
        *out = SPG_POLICY_CAP_MEMORY;
        return SPG_OK;
    }
    if (spg_sexpr_span_eq_cstr(input_n, input, span, "machine_process")) {
        *out = SPG_POLICY_CAP_MACHINE_PROCESS;
        return SPG_OK;
    }
    if (spg_sexpr_span_eq_cstr(input_n, input, span, "device")) {
        *out = SPG_POLICY_CAP_DEVICE;
        return SPG_OK;
    }
    return SPG_E_SCHEMA;
}

static enum spg_status parse_bool_span(const size_t input_n, const char input[],
                                       const struct spg_text_span span,
                                       bool *out) {
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

static uint32_t field_value_by_name(const size_t input_n, const char input[],
                                    const struct spg_sexpr_node nodes[static 1],
                                    const uint32_t cap_node,
                                    const char *name) {
    uint32_t field = spg_sexpr_first_child(nodes, cap_node);
    while (field != SPG_SEXPR_INVALID_INDEX) {
        if (nodes[field].kind == SPG_SEXPR_NODE_LIST) {
            const uint32_t field_name = spg_sexpr_first_child(nodes, field);
            if (field_name != SPG_SEXPR_INVALID_INDEX &&
                spg_sexpr_span_eq_cstr(input_n, input, nodes[field_name].span, name)) {
                return spg_sexpr_second_child(nodes, field);
            }
        }
        field = nodes[field].next_sibling;
    }
    return SPG_SEXPR_INVALID_INDEX;
}

static enum spg_status parse_capability(
    const size_t input_n, const char input[],
    const struct spg_sexpr_node nodes[static 1], const uint32_t cap_node,
    struct spg_policy_config *out, struct spg_policy_config_error *error) {
    if (out->capability_count >= SPG_POLICY_MAX_CAPABILITIES) {
        set_error(error, SPG_E_LIMIT, cap_node, nodes[cap_node].span.offset);
        return SPG_E_LIMIT;
    }

    const uint32_t name_value =
        field_value_by_name(input_n, input, nodes, cap_node, "name");
    const uint32_t kind_value =
        field_value_by_name(input_n, input, nodes, cap_node, "kind");
    const uint32_t enabled_value =
        field_value_by_name(input_n, input, nodes, cap_node, "enabled");
    const uint32_t budget_value =
        field_value_by_name(input_n, input, nodes, cap_node, "budget");

    if (name_value == SPG_SEXPR_INVALID_INDEX ||
        kind_value == SPG_SEXPR_INVALID_INDEX ||
        enabled_value == SPG_SEXPR_INVALID_INDEX ||
        budget_value == SPG_SEXPR_INVALID_INDEX ||
        nodes[name_value].kind != SPG_SEXPR_NODE_SYMBOL ||
        nodes[kind_value].kind != SPG_SEXPR_NODE_SYMBOL ||
        nodes[enabled_value].kind != SPG_SEXPR_NODE_SYMBOL ||
        nodes[budget_value].kind != SPG_SEXPR_NODE_SYMBOL) {
        set_error(error, SPG_E_SCHEMA, cap_node, nodes[cap_node].span.offset);
        return SPG_E_SCHEMA;
    }

    for (size_t i = 0u; i < out->capability_count; i += 1u) {
        if (out->capabilities[i].name.length == nodes[name_value].span.length &&
            memcmp(input + out->capabilities[i].name.offset,
                   input + nodes[name_value].span.offset,
                   nodes[name_value].span.length) == 0) {
            set_error(error, SPG_E_SCHEMA, name_value, nodes[name_value].span.offset);
            return SPG_E_SCHEMA;
        }
    }

    struct spg_policy_capability cap = {
        .name    = nodes[name_value].span,
        .kind    = SPG_POLICY_CAP_LOCAL_SHELL,
        .enabled = false,
        .budget  = 0u,
    };

    enum spg_status status =
        parse_cap_kind(input_n, input, nodes[kind_value].span, &cap.kind);
    if (status != SPG_OK) {
        set_error(error, status, kind_value, nodes[kind_value].span.offset);
        return status;
    }
    status =
        parse_bool_span(input_n, input, nodes[enabled_value].span, &cap.enabled);
    if (status != SPG_OK) {
        set_error(error, status, enabled_value, nodes[enabled_value].span.offset);
        return status;
    }
    status = spg_sexpr_parse_uint64_span(input_n, input, nodes[budget_value].span, &cap.budget);
    if (status != SPG_OK) {
        set_error(error, status, budget_value, nodes[budget_value].span.offset);
        return status;
    }

    out->capabilities[out->capability_count] = cap;
    out->capability_count += 1u;
    return SPG_OK;
}

static enum spg_status parse_capabilities(
    const size_t input_n, const char input[],
    const struct spg_sexpr_node nodes[static 1], const uint32_t policy_node,
    struct spg_policy_config *out, struct spg_policy_config_error *error) {
    const uint32_t field =
        find_field(input_n, input, nodes, policy_node, "capability");
    if (field == SPG_SEXPR_INVALID_INDEX) {
        out->capability_count = 0u;
        return SPG_OK;
    }
    uint32_t cap = spg_sexpr_second_child(nodes, field);
    while (cap != SPG_SEXPR_INVALID_INDEX) {
        if (nodes[cap].kind != SPG_SEXPR_NODE_LIST) {
            set_error(error, SPG_E_SCHEMA, cap, nodes[cap].span.offset);
            return SPG_E_SCHEMA;
        }
        const enum spg_status status =
            parse_capability(input_n, input, nodes, cap, out, error);
        if (status != SPG_OK) {
            return status;
        }
        cap = nodes[cap].next_sibling;
    }
    return SPG_OK;
}

enum spg_status spg_policy_config_load(
    const size_t input_n, const char input[], const size_t token_capacity,
    struct spg_sexpr_token tokens[static token_capacity],
    const size_t node_capacity,
    struct spg_sexpr_node nodes[static node_capacity],
    struct spg_policy_config *out, struct spg_policy_config_error *error) {
    if (input == nullptr || tokens == nullptr || nodes == nullptr ||
        out == nullptr || token_capacity == 0u || node_capacity == 0u) {
        set_error(error, SPG_E_INVALID_ARG, SPG_SEXPR_INVALID_INDEX, 0u);
        return SPG_E_INVALID_ARG;
    }
    *out = (struct spg_policy_config){};
    set_error(error, SPG_OK, SPG_SEXPR_INVALID_INDEX, 0u);

    struct spg_sexpr_error parse_error = {};
    size_t                 token_count = 0u;
    size_t                 node_count  = 0u;
    enum spg_status status =
        spg_sexpr_parse_text(input_n, input, token_capacity, tokens,
                             node_capacity, nodes, &token_count, &node_count,
                             &parse_error);
    if (status != SPG_OK) {
        set_error(error, status, SPG_SEXPR_INVALID_INDEX, parse_error.offset);
        return status;
    }
    if (node_count == 0u) {
        set_error(error, SPG_E_SCHEMA, SPG_SEXPR_INVALID_INDEX, 0u);
        return SPG_E_SCHEMA;
    }

    struct spg_schema_error schema_error = {};
    status = spg_schema_validate(input_n, input, node_count, nodes,
                                 &policy_schema, &schema_error);
    if (status != SPG_OK) {
        set_error(error, status, schema_error.node_index, schema_error.offset);
        return status;
    }

    const uint32_t policy_node = 0u;
    status = parse_network_default(input_n, input, nodes, policy_node,
                                   &out->network_default, error);
    if (status != SPG_OK) {
        return status;
    }
    status = parse_budgets(input_n, input, nodes, policy_node, &out->budgets, error);
    if (status != SPG_OK) {
        return status;
    }
    status = parse_capabilities(input_n, input, nodes, policy_node, out, error);
    if (status != SPG_OK) {
        return status;
    }
    return SPG_OK;
}
