#include "geistshell/risk.h"
#include "geistshell/sim_config.h"

#include <stdio.h>
#include <string.h>

static enum spg_status load_sim(const char *text, struct spg_sim_config *sim) {
    struct spg_sexpr_token     tokens[256];
    struct spg_sexpr_node      nodes[256];
    struct spg_sim_config_error error = {};
    return spg_sim_config_load(strlen(text), text, 256u, tokens, 256u, nodes,
                               sim, &error);
}

static int test_risk_components(void) {
    const char *text =
        "(scenario"
        " (host (id web) (criticality_bp 8000))"
        " (host (id db) (criticality_bp 10000))"
        " (service (id ssh_web) (host web) (name ssh) (port 22) (exposure_bp 5000))"
        " (account (id root_web) (host web) (username root) (enabled true))"
        " (credential (id key_root_web) (account root_web) (strength_bp 3000))"
        " (vulnerability (id cve_1) (service ssh_web) (severity_bp 7500) (patched false))"
        " (network_edge (from web) (to db) (reachability_bp 5000)))";
    struct spg_sim_config sim   = {};
    struct spg_risk_score score = {};
    if (load_sim(text, &sim) != SPG_OK) {
        return 1;
    }
    if (spg_risk_evaluate(&sim, &score) != SPG_OK) {
        return 1;
    }
    if (score.asset_component != 18000u) {
        return 1;
    }
    if (score.exposure_component != 4000u) {
        return 1;
    }
    if (score.vulnerability_component != 3000u) {
        return 1;
    }
    if (score.credential_component != 5600u) {
        return 1;
    }
    if (score.reachability_component != 4500u) {
        return 1;
    }
    return score.total == 35100u ? 0 : 1;
}

static int test_patched_and_disabled_do_not_contribute(void) {
    const char *text =
        "(scenario"
        " (host (id web) (criticality_bp 8000))"
        " (service (id ssh_web) (host web) (name ssh) (port 22) (exposure_bp 5000))"
        " (account (id root_web) (host web) (username root) (enabled false))"
        " (credential (id key_root_web) (account root_web) (strength_bp 0))"
        " (vulnerability (id cve_1) (service ssh_web) (severity_bp 10000) (patched true)))";
    struct spg_sim_config sim   = {};
    struct spg_risk_score score = {};
    if (load_sim(text, &sim) != SPG_OK) {
        return 1;
    }
    if (spg_risk_evaluate(&sim, &score) != SPG_OK) {
        return 1;
    }
    if (score.vulnerability_component != 0u ||
        score.credential_component != 0u) {
        return 1;
    }
    return score.total == score.asset_component + score.exposure_component ? 0
                                                                           : 1;
}

static int test_minimal_host_only(void) {
    const char *text = "(scenario (host (id solo) (criticality_bp 1234)))";
    struct spg_sim_config sim   = {};
    struct spg_risk_score score = {};
    if (load_sim(text, &sim) != SPG_OK) {
        return 1;
    }
    if (spg_risk_evaluate(&sim, &score) != SPG_OK) {
        return 1;
    }
    return score.asset_component == 1234u && score.total == 1234u ? 0 : 1;
}

static int test_invalid_args(void) {
    struct spg_sim_config sim = {};
    return spg_risk_evaluate(nullptr, nullptr) == SPG_E_INVALID_ARG &&
                   spg_risk_evaluate(&sim, nullptr) == SPG_E_INVALID_ARG
               ? 0
               : 1;
}

int main(void) {
    if (test_risk_components() != 0) {
        fprintf(stderr, "test_risk_components failed\n");
        return 1;
    }
    if (test_patched_and_disabled_do_not_contribute() != 0) {
        fprintf(stderr, "test_patched_and_disabled_do_not_contribute failed\n");
        return 1;
    }
    if (test_minimal_host_only() != 0) {
        fprintf(stderr, "test_minimal_host_only failed\n");
        return 1;
    }
    if (test_invalid_args() != 0) {
        fprintf(stderr, "test_invalid_args failed\n");
        return 1;
    }
    return 0;
}
