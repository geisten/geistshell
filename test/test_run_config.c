#include "geistshell/run_config.h"

#include <stdio.h>
#include <string.h>

static const char valid_run[] =
    "(run"
    " (model \"models/a.gguf\")"
    " (policy \"policy/lab.spg\")"
    " (scenario \"scenarios/lab.spg\")"
    " (corpus \"corpus/manifest.spg\")"
    " (journal \"runs/r1/run.sgj\")"
    " (seed 42)"
    " (budgets"
    "  (inference_steps 100)"
    "  (tokens 4096)"
    "  (shell_actions 8)"
    "  (sim_actions 250)"
    "  (wall_ms 60000)"
    "  (journal_bytes 1048576)"
    "  (risk_bp 10000)))";

static enum spg_status load_text(const char *text,
                                 struct spg_run_config *config,
                                 struct spg_run_config_error *error) {
    struct spg_sexpr_token tokens[128];
    struct spg_sexpr_node  nodes[128];
    return spg_run_config_load(strlen(text), text, 128u, tokens, 128u, nodes,
                               config, error);
}

static int span_eq(const char *text, const struct spg_text_span span,
                   const char *expected) {
    const size_t expected_n = strlen(expected);
    if (span.length != expected_n) {
        return 0;
    }
    return memcmp(text + span.offset, expected, expected_n) == 0 ? 1 : 0;
}

static int test_valid_run_config(void) {
    struct spg_run_config       config = {};
    struct spg_run_config_error error  = {};

    if (load_text(valid_run, &config, &error) != SPG_OK) {
        return 1;
    }
    if (!span_eq(valid_run, config.model_path, "models/a.gguf")) {
        return 1;
    }
    if (!span_eq(valid_run, config.policy_path, "policy/lab.spg")) {
        return 1;
    }
    if (!span_eq(valid_run, config.scenario_path, "scenarios/lab.spg")) {
        return 1;
    }
    if (!span_eq(valid_run, config.corpus_manifest_path,
                 "corpus/manifest.spg")) {
        return 1;
    }
    if (!span_eq(valid_run, config.journal_path, "runs/r1/run.sgj")) {
        return 1;
    }
    if (config.seed != 42u || config.budgets.inference_steps != 100u ||
        config.budgets.tokens != 4096u ||
        config.budgets.shell_actions != 8u ||
        config.budgets.sim_actions != 250u ||
        config.budgets.wall_ms != 60000u ||
        config.budgets.journal_bytes != 1048576u ||
        config.budgets.risk_bp != 10000u) {
        return 1;
    }
    return 0;
}

/* P1 (docs/LEARNING.md): the optional success criterion. Absent is valid and
 * leaves has_expect false; present parses the substring the finished run's
 * observation must contain. */
static int test_expect_absent(void) {
    struct spg_run_config       config = {};
    struct spg_run_config_error error  = {};
    if (load_text(valid_run, &config, &error) != SPG_OK) {
        return 1;
    }
    return config.has_expect ? 1 : 0;
}

static int test_expect_present(void) {
    const char *text =
        "(run"
        " (model \"models/a.gguf\")"
        " (policy \"policy/lab.spg\")"
        " (scenario \"scenarios/lab.spg\")"
        " (corpus \"corpus/manifest.spg\")"
        " (journal \"runs/r1/run.sgj\")"
        " (seed 42)"
        " (budgets"
        "  (inference_steps 100)"
        "  (tokens 4096)"
        "  (shell_actions 8)"
        "  (sim_actions 250)"
        "  (wall_ms 60000)"
        "  (journal_bytes 1048576)"
        "  (risk_bp 10000))"
        " (expect \"report generated\"))";
    struct spg_run_config       config = {};
    struct spg_run_config_error error  = {};
    if (load_text(text, &config, &error) != SPG_OK) {
        return 1;
    }
    if (!config.has_expect) {
        return 1;
    }
    return span_eq(text, config.expect_observation, "report generated") ? 0 : 1;
}

static int test_missing_required_field(void) {
    const char *text =
        "(run"
        " (model \"models/a.gguf\")"
        " (scenario \"scenarios/lab.spg\")"
        " (corpus \"corpus/manifest.spg\")"
        " (journal \"runs/r1/run.sgj\")"
        " (seed 42)"
        " (budgets"
        "  (inference_steps 100)"
        "  (tokens 4096)"
        "  (shell_actions 8)"
        "  (sim_actions 250)"
        "  (wall_ms 60000)"
        "  (journal_bytes 1048576)"
        "  (risk_bp 10000)))";
    struct spg_run_config       config = {};
    struct spg_run_config_error error  = {};
    const enum spg_status status = load_text(text, &config, &error);
    return status == SPG_E_SCHEMA && error.status == SPG_E_SCHEMA ? 0 : 1;
}

static int test_duplicate_budget(void) {
    const char *text =
        "(run"
        " (model \"models/a.gguf\")"
        " (policy \"policy/lab.spg\")"
        " (scenario \"scenarios/lab.spg\")"
        " (corpus \"corpus/manifest.spg\")"
        " (journal \"runs/r1/run.sgj\")"
        " (seed 42)"
        " (budgets"
        "  (inference_steps 100)"
        "  (tokens 4096)"
        "  (tokens 1)"
        "  (shell_actions 8)"
        "  (sim_actions 250)"
        "  (wall_ms 60000)"
        "  (journal_bytes 1048576)))";
    struct spg_run_config       config = {};
    struct spg_run_config_error error  = {};
    const enum spg_status status = load_text(text, &config, &error);
    return status == SPG_E_SCHEMA && error.status == SPG_E_SCHEMA ? 0 : 1;
}

static int test_unknown_budget(void) {
    const char *text =
        "(run"
        " (model \"models/a.gguf\")"
        " (policy \"policy/lab.spg\")"
        " (scenario \"scenarios/lab.spg\")"
        " (corpus \"corpus/manifest.spg\")"
        " (journal \"runs/r1/run.sgj\")"
        " (seed 42)"
        " (budgets"
        "  (inference_steps 100)"
        "  (tokens 4096)"
        "  (shell_actions 8)"
        "  (sim_actions 250)"
        "  (wall_ms 60000)"
        "  (journal_bytes 1048576)"
        "  (unknown 10000)))";
    struct spg_run_config       config = {};
    struct spg_run_config_error error  = {};
    const enum spg_status status = load_text(text, &config, &error);
    return status == SPG_E_SCHEMA && error.status == SPG_E_SCHEMA ? 0 : 1;
}

static int test_invalid_seed(void) {
    const char *text =
        "(run"
        " (model \"models/a.gguf\")"
        " (policy \"policy/lab.spg\")"
        " (scenario \"scenarios/lab.spg\")"
        " (corpus \"corpus/manifest.spg\")"
        " (journal \"runs/r1/run.sgj\")"
        " (seed no)"
        " (budgets"
        "  (inference_steps 100)"
        "  (tokens 4096)"
        "  (shell_actions 8)"
        "  (sim_actions 250)"
        "  (wall_ms 60000)"
        "  (journal_bytes 1048576)"
        "  (risk_bp 10000)))";
    struct spg_run_config       config = {};
    struct spg_run_config_error error  = {};
    const enum spg_status status = load_text(text, &config, &error);
    return status == SPG_E_FORMAT && error.status == SPG_E_FORMAT ? 0 : 1;
}

static int test_integer_overflow(void) {
    const char *text =
        "(run"
        " (model \"models/a.gguf\")"
        " (policy \"policy/lab.spg\")"
        " (scenario \"scenarios/lab.spg\")"
        " (corpus \"corpus/manifest.spg\")"
        " (journal \"runs/r1/run.sgj\")"
        " (seed 18446744073709551616)"
        " (budgets"
        "  (inference_steps 100)"
        "  (tokens 4096)"
        "  (shell_actions 8)"
        "  (sim_actions 250)"
        "  (wall_ms 60000)"
        "  (journal_bytes 1048576)"
        "  (risk_bp 10000)))";
    struct spg_run_config       config = {};
    struct spg_run_config_error error  = {};
    const enum spg_status status = load_text(text, &config, &error);
    return status == SPG_E_OVERFLOW && error.status == SPG_E_OVERFLOW ? 0 : 1;
}

static int test_token_capacity_limit(void) {
    struct spg_sexpr_token      tokens[4];
    struct spg_sexpr_node       nodes[128];
    struct spg_run_config       config = {};
    struct spg_run_config_error error  = {};
    const enum spg_status status =
        spg_run_config_load(strlen(valid_run), valid_run, 4u, tokens, 128u,
                            nodes, &config, &error);
    return status == SPG_E_LIMIT && error.status == SPG_E_LIMIT ? 0 : 1;
}

static int test_empty_input(void) {
    struct spg_sexpr_token      tokens[1];
    struct spg_sexpr_node       nodes[1];
    struct spg_run_config       config = {};
    struct spg_run_config_error error  = {};
    const enum spg_status status =
        spg_run_config_load(0u, "", 1u, tokens, 1u, nodes, &config, &error);
    return status == SPG_E_SCHEMA && error.status == SPG_E_SCHEMA ? 0 : 1;
}

static int test_invalid_args(void) {
    struct spg_sexpr_token      tokens[1];
    struct spg_sexpr_node       nodes[1];
    struct spg_run_config_error error = {};

    if (spg_run_config_load(0u, nullptr, 1u, tokens, 1u, nodes, nullptr,
                            &error) != SPG_E_INVALID_ARG) {
        return 1;
    }
    return 0;
}

int main(void) {
    if (test_valid_run_config() != 0) {
        fprintf(stderr, "test_valid_run_config failed\n");
        return 1;
    }
    if (test_expect_absent() != 0) {
        fprintf(stderr, "test_expect_absent failed\n");
        return 1;
    }
    if (test_expect_present() != 0) {
        fprintf(stderr, "test_expect_present failed\n");
        return 1;
    }
    if (test_missing_required_field() != 0) {
        fprintf(stderr, "test_missing_required_field failed\n");
        return 1;
    }
    if (test_duplicate_budget() != 0) {
        fprintf(stderr, "test_duplicate_budget failed\n");
        return 1;
    }
    if (test_unknown_budget() != 0) {
        fprintf(stderr, "test_unknown_budget failed\n");
        return 1;
    }
    if (test_invalid_seed() != 0) {
        fprintf(stderr, "test_invalid_seed failed\n");
        return 1;
    }
    if (test_integer_overflow() != 0) {
        fprintf(stderr, "test_integer_overflow failed\n");
        return 1;
    }
    if (test_token_capacity_limit() != 0) {
        fprintf(stderr, "test_token_capacity_limit failed\n");
        return 1;
    }
    if (test_empty_input() != 0) {
        fprintf(stderr, "test_empty_input failed\n");
        return 1;
    }
    if (test_invalid_args() != 0) {
        fprintf(stderr, "test_invalid_args failed\n");
        return 1;
    }
    return 0;
}
