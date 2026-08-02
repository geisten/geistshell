#include "geistshell/sim_config.h"

#include <stdio.h>
#include <string.h>

static const char valid_scenario[] =
    "(scenario"
    " (host (id web) (criticality_bp 9000))"
    " (host (id db) (criticality_bp 10000))"
    " (service (id ssh_web) (host web) (name ssh) (port 22) (exposure_bp 7000))"
    " (service (id pg_db) (host db) (name postgres) (port 5432) (exposure_bp 2000))"
    " (account (id root_web) (host web) (username root) (enabled true))"
    " (credential (id key_root_web) (account root_web) (strength_bp 3000))"
    " (vulnerability (id cve_1) (service ssh_web) (severity_bp 8500) (patched false))"
    " (network_edge (from web) (to db) (reachability_bp 5000)))";

static enum spg_status load_text(const char *text, struct spg_sim_config *config,
                                 struct spg_sim_config_error *error) {
    struct spg_sexpr_token tokens[256];
    struct spg_sexpr_node  nodes[256];
    return spg_sim_config_load(strlen(text), text, 256u, tokens, 256u, nodes,
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

static int test_valid_scenario(void) {
    struct spg_sim_config       config = {};
    struct spg_sim_config_error error  = {};
    if (load_text(valid_scenario, &config, &error) != SPG_OK) {
        return 1;
    }
    if (config.host_count != 2u || config.service_count != 2u ||
        config.account_count != 1u || config.credential_count != 1u ||
        config.vulnerability_count != 1u || config.network_edge_count != 1u) {
        return 1;
    }
    if (!span_eq(valid_scenario, config.hosts[0].id, "web") ||
        config.hosts[1].criticality_bp != 10000u) {
        return 1;
    }
    if (config.services[0].host_index != 0u || config.services[0].port != 22u ||
        config.services[1].host_index != 1u) {
        return 1;
    }
    if (config.accounts[0].host_index != 0u || !config.accounts[0].enabled) {
        return 1;
    }
    if (config.credentials[0].account_index != 0u ||
        config.credentials[0].strength_bp != 3000u) {
        return 1;
    }
    if (config.vulnerabilities[0].service_index != 0u ||
        config.vulnerabilities[0].patched) {
        return 1;
    }
    if (config.network_edges[0].from_host_index != 0u ||
        config.network_edges[0].to_host_index != 1u) {
        return 1;
    }
    return 0;
}

static int test_duplicate_host_id(void) {
    const char *text =
        "(scenario"
        " (host (id web) (criticality_bp 9000))"
        " (host (id web) (criticality_bp 1000)))";
    struct spg_sim_config       config = {};
    struct spg_sim_config_error error  = {};
    const enum spg_status status = load_text(text, &config, &error);
    return status == SPG_E_SCHEMA && error.status == SPG_E_SCHEMA ? 0 : 1;
}

static int test_service_unknown_host(void) {
    const char *text =
        "(scenario"
        " (host (id web) (criticality_bp 9000))"
        " (service (id ssh_web) (host missing) (name ssh) (port 22) (exposure_bp 7000)))";
    struct spg_sim_config       config = {};
    struct spg_sim_config_error error  = {};
    const enum spg_status status = load_text(text, &config, &error);
    return status == SPG_E_SCHEMA && error.status == SPG_E_SCHEMA ? 0 : 1;
}

static int test_account_unknown_host(void) {
    const char *text =
        "(scenario"
        " (host (id web) (criticality_bp 9000))"
        " (account (id root_web) (host missing) (username root) (enabled true)))";
    struct spg_sim_config       config = {};
    struct spg_sim_config_error error  = {};
    const enum spg_status status = load_text(text, &config, &error);
    return status == SPG_E_SCHEMA && error.status == SPG_E_SCHEMA ? 0 : 1;
}

static int test_credential_unknown_account(void) {
    const char *text =
        "(scenario"
        " (host (id web) (criticality_bp 9000))"
        " (credential (id key) (account missing) (strength_bp 3000)))";
    struct spg_sim_config       config = {};
    struct spg_sim_config_error error  = {};
    const enum spg_status status = load_text(text, &config, &error);
    return status == SPG_E_SCHEMA && error.status == SPG_E_SCHEMA ? 0 : 1;
}

static int test_vulnerability_unknown_service(void) {
    const char *text =
        "(scenario"
        " (host (id web) (criticality_bp 9000))"
        " (vulnerability (id cve_1) (service missing) (severity_bp 8500) (patched false)))";
    struct spg_sim_config       config = {};
    struct spg_sim_config_error error  = {};
    const enum spg_status status = load_text(text, &config, &error);
    return status == SPG_E_SCHEMA && error.status == SPG_E_SCHEMA ? 0 : 1;
}

static int test_network_edge_unknown_host(void) {
    const char *text =
        "(scenario"
        " (host (id web) (criticality_bp 9000))"
        " (network_edge (from web) (to missing) (reachability_bp 5000)))";
    struct spg_sim_config       config = {};
    struct spg_sim_config_error error  = {};
    const enum spg_status status = load_text(text, &config, &error);
    return status == SPG_E_SCHEMA && error.status == SPG_E_SCHEMA ? 0 : 1;
}

static int test_basis_points_limit(void) {
    const char *text = "(scenario (host (id web) (criticality_bp 10001)))";
    struct spg_sim_config       config = {};
    struct spg_sim_config_error error  = {};
    const enum spg_status status = load_text(text, &config, &error);
    return status == SPG_E_LIMIT && error.status == SPG_E_LIMIT ? 0 : 1;
}

static int test_port_limit(void) {
    const char *text =
        "(scenario"
        " (host (id web) (criticality_bp 9000))"
        " (service (id bad) (host web) (name x) (port 65536) (exposure_bp 1)))";
    struct spg_sim_config       config = {};
    struct spg_sim_config_error error  = {};
    const enum spg_status status = load_text(text, &config, &error);
    return status == SPG_E_LIMIT && error.status == SPG_E_LIMIT ? 0 : 1;
}

static int test_invalid_bool(void) {
    const char *text =
        "(scenario"
        " (host (id web) (criticality_bp 9000))"
        " (account (id root_web) (host web) (username root) (enabled yes)))";
    struct spg_sim_config       config = {};
    struct spg_sim_config_error error  = {};
    const enum spg_status status = load_text(text, &config, &error);
    return status == SPG_E_SCHEMA && error.status == SPG_E_SCHEMA ? 0 : 1;
}

static int test_empty_input(void) {
    struct spg_sexpr_token     tokens[1];
    struct spg_sexpr_node      nodes[1];
    struct spg_sim_config      config = {};
    struct spg_sim_config_error error = {};
    const enum spg_status status =
        spg_sim_config_load(0u, "", 1u, tokens, 1u, nodes, &config, &error);
    return status == SPG_E_SCHEMA && error.status == SPG_E_SCHEMA ? 0 : 1;
}

int main(void) {
    if (test_valid_scenario() != 0) {
        fprintf(stderr, "test_valid_scenario failed\n");
        return 1;
    }
    if (test_duplicate_host_id() != 0) {
        fprintf(stderr, "test_duplicate_host_id failed\n");
        return 1;
    }
    if (test_service_unknown_host() != 0) {
        fprintf(stderr, "test_service_unknown_host failed\n");
        return 1;
    }
    if (test_account_unknown_host() != 0) {
        fprintf(stderr, "test_account_unknown_host failed\n");
        return 1;
    }
    if (test_credential_unknown_account() != 0) {
        fprintf(stderr, "test_credential_unknown_account failed\n");
        return 1;
    }
    if (test_vulnerability_unknown_service() != 0) {
        fprintf(stderr, "test_vulnerability_unknown_service failed\n");
        return 1;
    }
    if (test_network_edge_unknown_host() != 0) {
        fprintf(stderr, "test_network_edge_unknown_host failed\n");
        return 1;
    }
    if (test_basis_points_limit() != 0) {
        fprintf(stderr, "test_basis_points_limit failed\n");
        return 1;
    }
    if (test_port_limit() != 0) {
        fprintf(stderr, "test_port_limit failed\n");
        return 1;
    }
    if (test_invalid_bool() != 0) {
        fprintf(stderr, "test_invalid_bool failed\n");
        return 1;
    }
    if (test_empty_input() != 0) {
        fprintf(stderr, "test_empty_input failed\n");
        return 1;
    }
    return 0;
}
