#include "geistshell/policy_config.h"

#include <stdio.h>
#include <string.h>

static const char valid_policy[] =
    "(policy"
    " (network_default deny)"
    " (budgets"
    "  (inference_steps 100)"
    "  (tokens 4096)"
    "  (shell_actions 8)"
    "  (sim_actions 250)"
    "  (wall_ms 60000))"
    " (capability"
    "  ((name build.run) (kind local_shell) (enabled true) (budget 8))"
    "  ((name auth_probe.ssh_publickey_single) (kind ssh_auth_probe)"
    "   (enabled false) (budget 1))"
    "  ((name sim.act) (kind simulator) (enabled true) (budget 250))))";

static enum spg_status load_text(const char *text,
                                 struct spg_policy_config *config,
                                 struct spg_policy_config_error *error) {
    struct spg_sexpr_token tokens[192];
    struct spg_sexpr_node  nodes[192];
    return spg_policy_config_load(strlen(text), text, 192u, tokens, 192u,
                                  nodes, config, error);
}

static int span_eq(const char *text, const struct spg_text_span span,
                   const char *expected) {
    const size_t expected_n = strlen(expected);
    if (span.length != expected_n) {
        return 0;
    }
    return memcmp(text + span.offset, expected, expected_n) == 0 ? 1 : 0;
}

static int test_valid_policy_config(void) {
    struct spg_policy_config       config = {};
    struct spg_policy_config_error error  = {};
    if (load_text(valid_policy, &config, &error) != SPG_OK) {
        return 1;
    }
    if (config.network_default != SPG_POLICY_NETWORK_DENY ||
        config.budgets.tokens != 4096u || config.capability_count != 3u) {
        return 1;
    }
    if (!span_eq(valid_policy, config.capabilities[0].name, "build.run") ||
        config.capabilities[0].kind != SPG_POLICY_CAP_LOCAL_SHELL ||
        !config.capabilities[0].enabled || config.capabilities[0].budget != 8u) {
        return 1;
    }
    if (config.capabilities[1].kind != SPG_POLICY_CAP_SSH_AUTH_PROBE ||
        config.capabilities[1].enabled) {
        return 1;
    }
    if (config.capabilities[2].kind != SPG_POLICY_CAP_SIMULATOR) {
        return 1;
    }
    return 0;
}

static int test_network_allow_is_explicit(void) {
    const char *text =
        "(policy"
        " (network_default allow)"
        " (budgets"
        "  (inference_steps 1)"
        "  (tokens 1)"
        "  (shell_actions 1)"
        "  (sim_actions 1)"
        "  (wall_ms 1)))";
    struct spg_policy_config       config = {};
    struct spg_policy_config_error error  = {};
    if (load_text(text, &config, &error) != SPG_OK) {
        return 1;
    }
    return config.network_default == SPG_POLICY_NETWORK_ALLOW &&
                   config.capability_count == 0u
               ? 0
               : 1;
}

static int test_invalid_network_default(void) {
    const char *text =
        "(policy"
        " (network_default maybe)"
        " (budgets"
        "  (inference_steps 1)"
        "  (tokens 1)"
        "  (shell_actions 1)"
        "  (sim_actions 1)"
        "  (wall_ms 1)))";
    struct spg_policy_config       config = {};
    struct spg_policy_config_error error  = {};
    const enum spg_status status = load_text(text, &config, &error);
    return status == SPG_E_SCHEMA && error.status == SPG_E_SCHEMA ? 0 : 1;
}

static int test_duplicate_capability_name(void) {
    const char *text =
        "(policy"
        " (network_default deny)"
        " (budgets"
        "  (inference_steps 1)"
        "  (tokens 1)"
        "  (shell_actions 1)"
        "  (sim_actions 1)"
        "  (wall_ms 1))"
        " (capability"
        "  ((name build.run) (kind local_shell) (enabled true) (budget 1))"
        "  ((name build.run) (kind local_shell) (enabled true) (budget 1))))";
    struct spg_policy_config       config = {};
    struct spg_policy_config_error error  = {};
    const enum spg_status status = load_text(text, &config, &error);
    return status == SPG_E_SCHEMA && error.status == SPG_E_SCHEMA ? 0 : 1;
}

static int test_invalid_capability_kind(void) {
    const char *text =
        "(policy"
        " (network_default deny)"
        " (budgets"
        "  (inference_steps 1)"
        "  (tokens 1)"
        "  (shell_actions 1)"
        "  (sim_actions 1)"
        "  (wall_ms 1))"
        " (capability"
        "  ((name build.run) (kind unknown) (enabled true) (budget 1))))";
    struct spg_policy_config       config = {};
    struct spg_policy_config_error error  = {};
    const enum spg_status status = load_text(text, &config, &error);
    return status == SPG_E_SCHEMA && error.status == SPG_E_SCHEMA ? 0 : 1;
}

static int test_invalid_enabled_value(void) {
    const char *text =
        "(policy"
        " (network_default deny)"
        " (budgets"
        "  (inference_steps 1)"
        "  (tokens 1)"
        "  (shell_actions 1)"
        "  (sim_actions 1)"
        "  (wall_ms 1))"
        " (capability"
        "  ((name build.run) (kind local_shell) (enabled yes) (budget 1))))";
    struct spg_policy_config       config = {};
    struct spg_policy_config_error error  = {};
    const enum spg_status status = load_text(text, &config, &error);
    return status == SPG_E_SCHEMA && error.status == SPG_E_SCHEMA ? 0 : 1;
}

static int test_invalid_budget_integer(void) {
    const char *text =
        "(policy"
        " (network_default deny)"
        " (budgets"
        "  (inference_steps 1)"
        "  (tokens nope)"
        "  (shell_actions 1)"
        "  (sim_actions 1)"
        "  (wall_ms 1)))";
    struct spg_policy_config       config = {};
    struct spg_policy_config_error error  = {};
    const enum spg_status status = load_text(text, &config, &error);
    return status == SPG_E_FORMAT && error.status == SPG_E_FORMAT ? 0 : 1;
}

static int test_missing_capability_field(void) {
    const char *text =
        "(policy"
        " (network_default deny)"
        " (budgets"
        "  (inference_steps 1)"
        "  (tokens 1)"
        "  (shell_actions 1)"
        "  (sim_actions 1)"
        "  (wall_ms 1))"
        " (capability"
        "  ((name build.run) (kind local_shell) (enabled true))))";
    struct spg_policy_config       config = {};
    struct spg_policy_config_error error  = {};
    const enum spg_status status = load_text(text, &config, &error);
    return status == SPG_E_SCHEMA && error.status == SPG_E_SCHEMA ? 0 : 1;
}

static int test_empty_input(void) {
    struct spg_sexpr_token        tokens[1];
    struct spg_sexpr_node         nodes[1];
    struct spg_policy_config      config = {};
    struct spg_policy_config_error error = {};
    const enum spg_status status =
        spg_policy_config_load(0u, "", 1u, tokens, 1u, nodes, &config, &error);
    return status == SPG_E_SCHEMA && error.status == SPG_E_SCHEMA ? 0 : 1;
}

int main(void) {
    if (test_valid_policy_config() != 0) {
        fprintf(stderr, "test_valid_policy_config failed\n");
        return 1;
    }
    if (test_network_allow_is_explicit() != 0) {
        fprintf(stderr, "test_network_allow_is_explicit failed\n");
        return 1;
    }
    if (test_invalid_network_default() != 0) {
        fprintf(stderr, "test_invalid_network_default failed\n");
        return 1;
    }
    if (test_duplicate_capability_name() != 0) {
        fprintf(stderr, "test_duplicate_capability_name failed\n");
        return 1;
    }
    if (test_invalid_capability_kind() != 0) {
        fprintf(stderr, "test_invalid_capability_kind failed\n");
        return 1;
    }
    if (test_invalid_enabled_value() != 0) {
        fprintf(stderr, "test_invalid_enabled_value failed\n");
        return 1;
    }
    if (test_invalid_budget_integer() != 0) {
        fprintf(stderr, "test_invalid_budget_integer failed\n");
        return 1;
    }
    if (test_missing_capability_field() != 0) {
        fprintf(stderr, "test_missing_capability_field failed\n");
        return 1;
    }
    if (test_empty_input() != 0) {
        fprintf(stderr, "test_empty_input failed\n");
        return 1;
    }
    return 0;
}
