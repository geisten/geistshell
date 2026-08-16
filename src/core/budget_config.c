#include "geistshell/run_config.h"

#include "geistshell/sexpr.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Single source of truth for the budget field names and where each one lands
 * in struct spg_run_budgets. Each name is matched exactly once, replacing the
 * former strcmp ladder + parallel name table in the two config loaders. */
struct budget_field {
    const char *name;
    size_t      offset;
    bool        optional; /* absent in config -> fallback default, not an error */
    uint64_t    fallback;
};

static const struct budget_field budget_fields[] = {
    {"inference_steps", offsetof(struct spg_run_budgets, inference_steps), false,
     0u},
    {"tokens", offsetof(struct spg_run_budgets, tokens), false, 0u},
    {"shell_actions", offsetof(struct spg_run_budgets, shell_actions), false,
     0u},
    {"sim_actions", offsetof(struct spg_run_budgets, sim_actions), false, 0u},
    {"memory_actions", offsetof(struct spg_run_budgets, memory_actions), true,
     UINT64_MAX},
    /* Optional like memory_actions: a config written before machine actions
     * existed must keep loading, and an absent budget means unlimited rather
     * than zero — zero would silently disable a capability the operator
     * explicitly enabled. */
    {"machine_actions", offsetof(struct spg_run_budgets, machine_actions), true,
     UINT64_MAX},
    {"wall_ms", offsetof(struct spg_run_budgets, wall_ms), false, 0u},
};
/* journal_bytes and risk_bp were declared, rendered into every prompt, and
 * enforced nowhere; removed 2026-08-09. Unknown names fall through the table
 * below and fail SPG_E_SCHEMA, so an old config breaks loudly rather than
 * silently paying context bytes for a number nothing reads. */

static constexpr size_t budget_field_count =
    sizeof(budget_fields) / sizeof(budget_fields[0]);

static enum spg_status parse_budget_item(
    const size_t input_n, const char input[],
    const struct spg_sexpr_node nodes[static 1], const uint32_t item,
    struct spg_run_budgets *budgets, bool seen[static budget_field_count],
    uint32_t *err_node, size_t *err_offset) {
    const uint32_t name_node  = spg_sexpr_first_child(nodes, item);
    const uint32_t value_node = spg_sexpr_second_child(nodes, item);
    if (name_node == SPG_SEXPR_INVALID_INDEX ||
        value_node == SPG_SEXPR_INVALID_INDEX ||
        nodes[value_node].next_sibling != SPG_SEXPR_INVALID_INDEX ||
        nodes[name_node].kind != SPG_SEXPR_NODE_SYMBOL ||
        nodes[value_node].kind != SPG_SEXPR_NODE_SYMBOL) {
        *err_node   = item;
        *err_offset = nodes[item].span.offset;
        return SPG_E_SCHEMA;
    }

    uint64_t value = 0u;
    const enum spg_status status =
        spg_sexpr_parse_uint64_span(input_n, input, nodes[value_node].span, &value);
    if (status != SPG_OK) {
        *err_node   = value_node;
        *err_offset = nodes[value_node].span.offset;
        return status;
    }

    for (size_t i = 0u; i < budget_field_count; i += 1u) {
        if (spg_sexpr_span_eq_cstr(input_n, input, nodes[name_node].span,
                                   budget_fields[i].name)) {
            if (seen[i]) {
                *err_node   = item;
                *err_offset = nodes[item].span.offset;
                return SPG_E_SCHEMA;
            }
            seen[i] = true;
            *(uint64_t *)((char *)budgets + budget_fields[i].offset) = value;
            return SPG_OK;
        }
    }

    *err_node   = name_node;
    *err_offset = nodes[name_node].span.offset;
    return SPG_E_SCHEMA;
}

enum spg_status spg_run_budgets_parse(
    const size_t input_n, const char input[],
    const struct spg_sexpr_node nodes[static 1], const uint32_t budgets_field,
    struct spg_run_budgets *out, uint32_t *err_node, size_t *err_offset) {
    bool seen[budget_field_count] = {};
    *out                          = (struct spg_run_budgets){};

    uint32_t item = spg_sexpr_second_child(nodes, budgets_field);
    while (item != SPG_SEXPR_INVALID_INDEX) {
        if (nodes[item].kind != SPG_SEXPR_NODE_LIST) {
            *err_node   = item;
            *err_offset = nodes[item].span.offset;
            return SPG_E_SCHEMA;
        }
        const enum spg_status status = parse_budget_item(
            input_n, input, nodes, item, out, seen, err_node, err_offset);
        if (status != SPG_OK) {
            return status;
        }
        item = nodes[item].next_sibling;
    }

    for (size_t i = 0u; i < budget_field_count; i += 1u) {
        if (seen[i]) {
            continue;
        }
        if (budget_fields[i].optional) {
            *(uint64_t *)((char *)out + budget_fields[i].offset) =
                budget_fields[i].fallback;
            continue;
        }
        *err_node   = budgets_field;
        *err_offset = nodes[budgets_field].span.offset;
        return SPG_E_SCHEMA;
    }
    return SPG_OK;
}
