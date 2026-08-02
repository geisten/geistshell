#ifndef GEISTSHELL_SCHEMA_H
#define GEISTSHELL_SCHEMA_H

#include "geistshell/sexpr.h"
#include "geistshell/status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum spg_schema_value_kind {
    SPG_SCHEMA_VALUE_ANY = 0,
    SPG_SCHEMA_VALUE_SYMBOL,
    SPG_SCHEMA_VALUE_STRING,
    SPG_SCHEMA_VALUE_LIST,
};

struct spg_schema_field_rule {
    const char                *name;
    enum spg_schema_value_kind value_kind;
    size_t                     min_values;
    size_t                     max_values;
    bool                       required;
    bool                       unique;
};

struct spg_schema_form_rule {
    const char *name;

    size_t                              field_rule_count;
    const struct spg_schema_field_rule *field_rules;

    bool   allow_unknown_fields;
    size_t min_fields;
    size_t max_fields;
};

struct spg_schema {
    size_t                             form_rule_count;
    const struct spg_schema_form_rule *form_rules;

    bool   allow_unknown_top_level_forms;
    size_t max_top_level_forms;
    size_t max_depth;
};

struct spg_schema_error {
    enum spg_status status;
    uint32_t        node_index;
    size_t          offset;
};

[[nodiscard]] enum spg_status
spg_schema_validate(size_t input_n, const char input[], size_t node_n,
                    const struct spg_sexpr_node nodes[],
                    const struct spg_schema *schema,
                    struct spg_schema_error *error);

#ifdef __cplusplus
}
#endif

#endif
