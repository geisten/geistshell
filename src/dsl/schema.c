#include "geistshell/schema.h"

#include <string.h>

static void set_schema_error(struct spg_schema_error *error,
                             const enum spg_status    status,
                             const uint32_t           node_index,
                             const size_t             offset) {
    if (error == nullptr) {
        return;
    }
    error->status     = status;
    error->node_index = node_index;
    error->offset     = offset;
}

static uint32_t form_name_node(const struct spg_sexpr_node *form) {
    return form->first_child;
}

static const struct spg_schema_form_rule *
find_form_rule(const size_t input_n, const char input[],
               const struct spg_sexpr_node nodes[static 1],
               const uint32_t form_index, const struct spg_schema *schema) {
    const uint32_t name_index = form_name_node(&nodes[form_index]);
    if (name_index == SPG_SEXPR_INVALID_INDEX ||
        nodes[name_index].kind != SPG_SEXPR_NODE_SYMBOL) {
        return nullptr;
    }
    for (size_t i = 0u; i < schema->form_rule_count; i += 1u) {
        if (spg_sexpr_span_eq_cstr(input_n, input, nodes[name_index].span,
                         schema->form_rules[i].name)) {
            return &schema->form_rules[i];
        }
    }
    return nullptr;
}

static const struct spg_schema_field_rule *
find_field_rule(const size_t input_n, const char input[],
                const struct spg_sexpr_node nodes[static 1],
                const uint32_t field_index,
                const struct spg_schema_form_rule *form_rule) {
    const uint32_t name_index = form_name_node(&nodes[field_index]);
    if (name_index == SPG_SEXPR_INVALID_INDEX ||
        nodes[name_index].kind != SPG_SEXPR_NODE_SYMBOL) {
        return nullptr;
    }
    for (size_t i = 0u; i < form_rule->field_rule_count; i += 1u) {
        if (spg_sexpr_span_eq_cstr(input_n, input, nodes[name_index].span,
                         form_rule->field_rules[i].name)) {
            return &form_rule->field_rules[i];
        }
    }
    return nullptr;
}

static size_t child_count(const struct spg_sexpr_node nodes[static 1],
                          const uint32_t parent) {
    size_t   count  = 0u;
    uint32_t cursor = nodes[parent].first_child;
    while (cursor != SPG_SEXPR_INVALID_INDEX) {
        count += 1u;
        cursor = nodes[cursor].next_sibling;
    }
    return count;
}

static enum spg_status validate_depth(
    const size_t node_n, const struct spg_sexpr_node nodes[static node_n],
    const struct spg_schema *schema, struct spg_schema_error *error) {
    if (schema->max_depth == 0u) {
        return SPG_OK;
    }
    for (size_t i = 0u; i < node_n; i += 1u) {
        size_t   depth  = 1u;
        uint32_t parent = nodes[i].parent;
        while (parent != SPG_SEXPR_INVALID_INDEX) {
            depth += 1u;
            if (depth > schema->max_depth) {
                set_schema_error(error, SPG_E_SCHEMA, (uint32_t)i,
                                 nodes[i].span.offset);
                return SPG_E_SCHEMA;
            }
            parent = nodes[parent].parent;
        }
    }
    return SPG_OK;
}

static bool value_kind_matches(const enum spg_schema_value_kind expected,
                               const enum spg_sexpr_node_kind   actual) {
    if (expected == SPG_SCHEMA_VALUE_ANY) {
        return true;
    }
    if (expected == SPG_SCHEMA_VALUE_SYMBOL) {
        return actual == SPG_SEXPR_NODE_SYMBOL;
    }
    if (expected == SPG_SCHEMA_VALUE_STRING) {
        return actual == SPG_SEXPR_NODE_STRING;
    }
    if (expected == SPG_SCHEMA_VALUE_LIST) {
        return actual == SPG_SEXPR_NODE_LIST;
    }
    return false;
}

static enum spg_status validate_field_values(
    const struct spg_sexpr_node nodes[static 1], const uint32_t field_index,
    const struct spg_schema_field_rule *field_rule,
    struct spg_schema_error *error) {
    const size_t total_children = child_count(nodes, field_index);
    if (total_children == 0u) {
        set_schema_error(error, SPG_E_SCHEMA, field_index,
                         nodes[field_index].span.offset);
        return SPG_E_SCHEMA;
    }
    const size_t value_count = total_children - 1u;
    if (value_count < field_rule->min_values ||
        value_count > field_rule->max_values) {
        set_schema_error(error, SPG_E_SCHEMA, field_index,
                         nodes[field_index].span.offset);
        return SPG_E_SCHEMA;
    }

    uint32_t value = nodes[nodes[field_index].first_child].next_sibling;
    while (value != SPG_SEXPR_INVALID_INDEX) {
        if (!value_kind_matches(field_rule->value_kind, nodes[value].kind)) {
            set_schema_error(error, SPG_E_SCHEMA, value, nodes[value].span.offset);
            return SPG_E_SCHEMA;
        }
        value = nodes[value].next_sibling;
    }
    return SPG_OK;
}

static enum spg_status validate_form_fields(
    const size_t input_n, const char input[],
    const struct spg_sexpr_node nodes[static 1], const uint32_t form_index,
    const struct spg_schema_form_rule *form_rule,
    struct spg_schema_error *error) {
    const size_t total_children = child_count(nodes, form_index);
    if (total_children == 0u) {
        set_schema_error(error, SPG_E_SCHEMA, form_index,
                         nodes[form_index].span.offset);
        return SPG_E_SCHEMA;
    }

    const size_t field_count = total_children - 1u;
    if (field_count < form_rule->min_fields || field_count > form_rule->max_fields) {
        set_schema_error(error, SPG_E_SCHEMA, form_index,
                         nodes[form_index].span.offset);
        return SPG_E_SCHEMA;
    }

    for (size_t rule_i = 0u; rule_i < form_rule->field_rule_count; rule_i += 1u) {
        const struct spg_schema_field_rule *field_rule =
            &form_rule->field_rules[rule_i];
        size_t matches = 0u;

        uint32_t field = nodes[nodes[form_index].first_child].next_sibling;
        while (field != SPG_SEXPR_INVALID_INDEX) {
            if (nodes[field].kind != SPG_SEXPR_NODE_LIST) {
                set_schema_error(error, SPG_E_SCHEMA, field, nodes[field].span.offset);
                return SPG_E_SCHEMA;
            }
            const uint32_t field_name = form_name_node(&nodes[field]);
            if (field_name != SPG_SEXPR_INVALID_INDEX &&
                nodes[field_name].kind == SPG_SEXPR_NODE_SYMBOL &&
                spg_sexpr_span_eq_cstr(input_n, input, nodes[field_name].span, field_rule->name)) {
                matches += 1u;
                if (field_rule->unique && matches > 1u) {
                    set_schema_error(error, SPG_E_SCHEMA, field,
                                     nodes[field].span.offset);
                    return SPG_E_SCHEMA;
                }
                const enum spg_status status =
                    validate_field_values(nodes, field, field_rule, error);
                if (status != SPG_OK) {
                    return status;
                }
            }
            field = nodes[field].next_sibling;
        }

        if (field_rule->required && matches == 0u) {
            set_schema_error(error, SPG_E_SCHEMA, form_index,
                             nodes[form_index].span.offset);
            return SPG_E_SCHEMA;
        }
    }

    if (!form_rule->allow_unknown_fields) {
        uint32_t field = nodes[nodes[form_index].first_child].next_sibling;
        while (field != SPG_SEXPR_INVALID_INDEX) {
            if (nodes[field].kind != SPG_SEXPR_NODE_LIST ||
                find_field_rule(input_n, input, nodes, field, form_rule) == nullptr) {
                set_schema_error(error, SPG_E_SCHEMA, field,
                                 nodes[field].span.offset);
                return SPG_E_SCHEMA;
            }
            field = nodes[field].next_sibling;
        }
    }

    return SPG_OK;
}

enum spg_status
spg_schema_validate(const size_t input_n, const char input[],
                    const size_t node_n,
                    const struct spg_sexpr_node nodes[],
                    const struct spg_schema *schema,
                    struct spg_schema_error *error) {
    if (input == nullptr || nodes == nullptr || schema == nullptr ||
        (schema->form_rule_count > 0u && schema->form_rules == nullptr)) {
        set_schema_error(error, SPG_E_INVALID_ARG, SPG_SEXPR_INVALID_INDEX, 0u);
        return SPG_E_INVALID_ARG;
    }
    set_schema_error(error, SPG_OK, SPG_SEXPR_INVALID_INDEX, 0u);

    const enum spg_status depth_status =
        validate_depth(node_n, nodes, schema, error);
    if (depth_status != SPG_OK) {
        return depth_status;
    }

    size_t top_level_count = 0u;
    for (size_t i = 0u; i < node_n; i += 1u) {
        if (nodes[i].parent != SPG_SEXPR_INVALID_INDEX) {
            continue;
        }
        top_level_count += 1u;
        if (schema->max_top_level_forms > 0u &&
            top_level_count > schema->max_top_level_forms) {
            set_schema_error(error, SPG_E_SCHEMA, (uint32_t)i,
                             nodes[i].span.offset);
            return SPG_E_SCHEMA;
        }
        if (nodes[i].kind != SPG_SEXPR_NODE_LIST) {
            set_schema_error(error, SPG_E_SCHEMA, (uint32_t)i,
                             nodes[i].span.offset);
            return SPG_E_SCHEMA;
        }

        const struct spg_schema_form_rule *form_rule =
            find_form_rule(input_n, input, nodes, (uint32_t)i, schema);
        if (form_rule == nullptr) {
            if (schema->allow_unknown_top_level_forms) {
                continue;
            }
            set_schema_error(error, SPG_E_SCHEMA, (uint32_t)i,
                             nodes[i].span.offset);
            return SPG_E_SCHEMA;
        }
        const enum spg_status status =
            validate_form_fields(input_n, input, nodes, (uint32_t)i, form_rule,
                                 error);
        if (status != SPG_OK) {
            return status;
        }
    }

    return SPG_OK;
}
