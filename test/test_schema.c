#include "geistshell/schema.h"
#include "geistshell/sexpr.h"

#include <stdio.h>
#include <string.h>

static const struct spg_schema_field_rule run_fields[] = {
    {
        .name       = "model",
        .value_kind = SPG_SCHEMA_VALUE_STRING,
        .min_values = 1u,
        .max_values = 1u,
        .required   = true,
        .unique     = true,
    },
    {
        .name       = "policy",
        .value_kind = SPG_SCHEMA_VALUE_STRING,
        .min_values = 1u,
        .max_values = 1u,
        .required   = true,
        .unique     = true,
    },
    {
        .name       = "seed",
        .value_kind = SPG_SCHEMA_VALUE_SYMBOL,
        .min_values = 1u,
        .max_values = 1u,
        .required   = false,
        .unique     = true,
    },
};

static const struct spg_schema_form_rule forms[] = {
    {
        .name                 = "run",
        .field_rule_count     = sizeof(run_fields) / sizeof(run_fields[0]),
        .field_rules          = run_fields,
        .allow_unknown_fields = false,
        .min_fields           = 2u,
        .max_fields           = 3u,
    },
};

static const struct spg_schema run_schema = {
    .form_rule_count               = sizeof(forms) / sizeof(forms[0]),
    .form_rules                    = forms,
    .allow_unknown_top_level_forms = false,
    .max_top_level_forms           = 1u,
    .max_depth                     = 4u,
};

static enum spg_status validate_text(const char *text,
                                     struct spg_schema_error *schema_error) {
    struct spg_sexpr_token tokens[64];
    struct spg_sexpr_node  nodes[64];
    struct spg_sexpr_error parse_error = {};
    size_t                 token_count = 0u;
    size_t                 node_count  = 0u;

    enum spg_status status =
        spg_sexpr_parse_text(strlen(text), text, 64u, tokens, 64u, nodes,
                             &token_count, &node_count, &parse_error);
    if (status != SPG_OK) {
        return status;
    }
    return spg_schema_validate(strlen(text), text, node_count, nodes,
                               &run_schema, schema_error);
}

static int test_valid_run_schema(void) {
    struct spg_schema_error error = {};
    const enum spg_status status =
        validate_text("(run (model \"m.gguf\") (policy \"lab.spg\") (seed 7))",
                      &error);
    return status == SPG_OK && error.status == SPG_OK ? 0 : 1;
}

static int test_missing_required_field(void) {
    struct spg_schema_error error = {};
    const enum spg_status status =
        validate_text("(run (model \"m.gguf\"))", &error);
    return status == SPG_E_SCHEMA && error.status == SPG_E_SCHEMA ? 0 : 1;
}

static int test_duplicate_unique_field(void) {
    struct spg_schema_error error = {};
    const enum spg_status status = validate_text(
        "(run (model \"a.gguf\") (model \"b.gguf\") (policy \"lab.spg\"))",
        &error);
    return status == SPG_E_SCHEMA && error.status == SPG_E_SCHEMA ? 0 : 1;
}

static int test_unknown_field(void) {
    struct spg_schema_error error = {};
    const enum spg_status status = validate_text(
        "(run (model \"m.gguf\") (policy \"lab.spg\") (extra yes))", &error);
    return status == SPG_E_SCHEMA && error.status == SPG_E_SCHEMA ? 0 : 1;
}

static int test_wrong_value_kind(void) {
    struct spg_schema_error error = {};
    const enum spg_status status =
        validate_text("(run (model m.gguf) (policy \"lab.spg\"))", &error);
    return status == SPG_E_SCHEMA && error.status == SPG_E_SCHEMA ? 0 : 1;
}

static int test_wrong_arity(void) {
    struct spg_schema_error error = {};
    const enum spg_status status =
        validate_text("(run (model \"m.gguf\" \"x\") (policy \"lab.spg\"))",
                      &error);
    return status == SPG_E_SCHEMA && error.status == SPG_E_SCHEMA ? 0 : 1;
}

static int test_unknown_top_level_form(void) {
    struct spg_schema_error error = {};
    const enum spg_status status =
        validate_text("(policy (name \"lab\"))", &error);
    return status == SPG_E_SCHEMA && error.status == SPG_E_SCHEMA ? 0 : 1;
}

static int test_too_many_top_level_forms(void) {
    struct spg_schema_error error = {};
    const enum spg_status status = validate_text(
        "(run (model \"m.gguf\") (policy \"lab.spg\"))"
        "(run (model \"n.gguf\") (policy \"lab.spg\"))",
        &error);
    return status == SPG_E_SCHEMA && error.status == SPG_E_SCHEMA ? 0 : 1;
}

static int test_depth_limit(void) {
    struct spg_schema_error error = {};
    const enum spg_status status = validate_text(
        "(run (model \"m.gguf\") (policy \"lab.spg\") (seed (nested 7)))",
        &error);
    return status == SPG_E_SCHEMA && error.status == SPG_E_SCHEMA ? 0 : 1;
}

static int test_invalid_args(void) {
    struct spg_sexpr_node   nodes[1];
    struct spg_schema_error error = {};

    if (spg_schema_validate(0u, nullptr, 1u, nodes, &run_schema, &error) !=
        SPG_E_INVALID_ARG) {
        return 1;
    }
    if (spg_schema_validate(0u, "", 1u, nodes, nullptr, &error) !=
        SPG_E_INVALID_ARG) {
        return 1;
    }
    return 0;
}

int main(void) {
    if (test_valid_run_schema() != 0) {
        fprintf(stderr, "test_valid_run_schema failed\n");
        return 1;
    }
    if (test_missing_required_field() != 0) {
        fprintf(stderr, "test_missing_required_field failed\n");
        return 1;
    }
    if (test_duplicate_unique_field() != 0) {
        fprintf(stderr, "test_duplicate_unique_field failed\n");
        return 1;
    }
    if (test_unknown_field() != 0) {
        fprintf(stderr, "test_unknown_field failed\n");
        return 1;
    }
    if (test_wrong_value_kind() != 0) {
        fprintf(stderr, "test_wrong_value_kind failed\n");
        return 1;
    }
    if (test_wrong_arity() != 0) {
        fprintf(stderr, "test_wrong_arity failed\n");
        return 1;
    }
    if (test_unknown_top_level_form() != 0) {
        fprintf(stderr, "test_unknown_top_level_form failed\n");
        return 1;
    }
    if (test_too_many_top_level_forms() != 0) {
        fprintf(stderr, "test_too_many_top_level_forms failed\n");
        return 1;
    }
    if (test_depth_limit() != 0) {
        fprintf(stderr, "test_depth_limit failed\n");
        return 1;
    }
    if (test_invalid_args() != 0) {
        fprintf(stderr, "test_invalid_args failed\n");
        return 1;
    }
    return 0;
}
