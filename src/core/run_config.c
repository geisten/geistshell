#include "geistshell/run_config.h"

#include "geistshell/schema.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static const struct spg_schema_field_rule run_fields[] = {
    {.name       = "model",
     .value_kind = SPG_SCHEMA_VALUE_STRING,
     .min_values = 1u,
     .max_values = 1u,
     .required   = true,
     .unique     = true},
    {.name       = "policy",
     .value_kind = SPG_SCHEMA_VALUE_STRING,
     .min_values = 1u,
     .max_values = 1u,
     .required   = true,
     .unique     = true},
    {.name       = "scenario",
     .value_kind = SPG_SCHEMA_VALUE_STRING,
     .min_values = 1u,
     .max_values = 1u,
     .required   = true,
     .unique     = true},
    {.name       = "corpus",
     .value_kind = SPG_SCHEMA_VALUE_STRING,
     .min_values = 1u,
     .max_values = 1u,
     .required   = true,
     .unique     = true},
    {.name       = "journal",
     .value_kind = SPG_SCHEMA_VALUE_STRING,
     .min_values = 1u,
     .max_values = 1u,
     .required   = true,
     .unique     = true},
    {.name       = "seed",
     .value_kind = SPG_SCHEMA_VALUE_SYMBOL,
     .min_values = 1u,
     .max_values = 1u,
     .required   = true,
     .unique     = true},
    {.name       = "budgets",
     .value_kind = SPG_SCHEMA_VALUE_LIST,
     .min_values = 7u,
     .max_values = 8u, /* 7 required + optional memory_actions */
     .required   = true,
     .unique     = true},
};

static const struct spg_schema_form_rule run_forms[] = {
    {.name                 = "run",
     .field_rule_count     = sizeof(run_fields) / sizeof(run_fields[0]),
     .field_rules          = run_fields,
     .allow_unknown_fields = false,
     .min_fields           = 7u,
     .max_fields           = 7u},
};

static const struct spg_schema run_schema = {
    .form_rule_count               = sizeof(run_forms) / sizeof(run_forms[0]),
    .form_rules                    = run_forms,
    .allow_unknown_top_level_forms = false,
    .max_top_level_forms           = 1u,
    .max_depth                     = 4u,
};

static void set_error(struct spg_run_config_error *error,
                      const enum spg_status        status,
                      const uint32_t               node_index,
                      const size_t                 offset) {
    if (error == nullptr) {
        return;
    }
    error->status     = status;
    error->node_index = node_index;
    error->offset     = offset;
}

static uint32_t find_field(const size_t input_n, const char input[],
                           const struct spg_sexpr_node nodes[static 1],
                           const uint32_t run_node, const char *name) {
    const uint32_t form_name = spg_sexpr_first_child(nodes, run_node);
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

static enum spg_status string_value_span(
    const struct spg_sexpr_node nodes[static 1], const uint32_t field,
    struct spg_text_span *out) {
    const uint32_t value = spg_sexpr_second_child(nodes, field);
    if (value == SPG_SEXPR_INVALID_INDEX ||
        !spg_sexpr_string_payload_span(&nodes[value], out)) {
        return SPG_E_SCHEMA;
    }
    return SPG_OK;
}

static enum spg_status uint64_field_value(
    const size_t input_n, const char input[],
    const struct spg_sexpr_node nodes[static 1], const uint32_t field,
    uint64_t *out) {
    const uint32_t value = spg_sexpr_second_child(nodes, field);
    if (value == SPG_SEXPR_INVALID_INDEX ||
        nodes[value].kind != SPG_SEXPR_NODE_SYMBOL) {
        return SPG_E_SCHEMA;
    }
    return spg_sexpr_parse_uint64_span(input_n, input, nodes[value].span, out);
}

static enum spg_status assign_path_field(
    const size_t input_n, const char input[],
    const struct spg_sexpr_node nodes[static 1], const uint32_t run_node,
    const char *name, struct spg_text_span *out,
    struct spg_run_config_error *error) {
    const uint32_t field = find_field(input_n, input, nodes, run_node, name);
    if (field == SPG_SEXPR_INVALID_INDEX) {
        set_error(error, SPG_E_SCHEMA, run_node, nodes[run_node].span.offset);
        return SPG_E_SCHEMA;
    }
    const enum spg_status status = string_value_span(nodes, field, out);
    if (status != SPG_OK) {
        set_error(error, status, field, nodes[field].span.offset);
        return status;
    }
    return SPG_OK;
}

static enum spg_status parse_budgets(
    const size_t input_n, const char input[],
    const struct spg_sexpr_node nodes[static 1], const uint32_t run_node,
    struct spg_run_budgets *budgets, struct spg_run_config_error *error) {
    const uint32_t field = find_field(input_n, input, nodes, run_node, "budgets");
    if (field == SPG_SEXPR_INVALID_INDEX) {
        set_error(error, SPG_E_SCHEMA, run_node, nodes[run_node].span.offset);
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

enum spg_status spg_run_config_load(
    const size_t input_n, const char input[], const size_t token_capacity,
    struct spg_sexpr_token tokens[static token_capacity],
    const size_t node_capacity,
    struct spg_sexpr_node nodes[static node_capacity],
    struct spg_run_config *out, struct spg_run_config_error *error) {
    if (input == nullptr || tokens == nullptr || nodes == nullptr ||
        out == nullptr || token_capacity == 0u || node_capacity == 0u) {
        set_error(error, SPG_E_INVALID_ARG, SPG_SEXPR_INVALID_INDEX, 0u);
        return SPG_E_INVALID_ARG;
    }
    *out = (struct spg_run_config){};
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
    status = spg_schema_validate(input_n, input, node_count, nodes, &run_schema,
                                 &schema_error);
    if (status != SPG_OK) {
        set_error(error, status, schema_error.node_index, schema_error.offset);
        return status;
    }

    const uint32_t run_node = 0u;
    status = assign_path_field(input_n, input, nodes, run_node, "model",
                               &out->model_path, error);
    if (status != SPG_OK) {
        return status;
    }
    status = assign_path_field(input_n, input, nodes, run_node, "policy",
                               &out->policy_path, error);
    if (status != SPG_OK) {
        return status;
    }
    status = assign_path_field(input_n, input, nodes, run_node, "scenario",
                               &out->scenario_path, error);
    if (status != SPG_OK) {
        return status;
    }
    status = assign_path_field(input_n, input, nodes, run_node, "corpus",
                               &out->corpus_manifest_path, error);
    if (status != SPG_OK) {
        return status;
    }
    status = assign_path_field(input_n, input, nodes, run_node, "journal",
                               &out->journal_path, error);
    if (status != SPG_OK) {
        return status;
    }

    const uint32_t seed_field = find_field(input_n, input, nodes, run_node, "seed");
    status = uint64_field_value(input_n, input, nodes, seed_field, &out->seed);
    if (status != SPG_OK) {
        set_error(error, status, seed_field, nodes[seed_field].span.offset);
        return status;
    }

    status = parse_budgets(input_n, input, nodes, run_node, &out->budgets, error);
    if (status != SPG_OK) {
        return status;
    }

    return SPG_OK;
}
