#include "geistshell/risk.h"

#include <stdint.h>

static uint64_t bp_mul(const uint32_t a, const uint32_t b) {
    return ((uint64_t)a * (uint64_t)b) / SPG_SIM_MAX_BASIS_POINTS;
}

enum spg_status spg_risk_evaluate(const struct spg_sim_config *sim,
                                  struct spg_risk_score *out) {
    if (sim == nullptr || out == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out = (struct spg_risk_score){};

    for (size_t i = 0u; i < sim->host_count; i += 1u) {
        out->asset_component += sim->hosts[i].criticality_bp;
    }

    for (size_t i = 0u; i < sim->service_count; i += 1u) {
        const struct spg_sim_service *service = &sim->services[i];
        const struct spg_sim_host    *host    = &sim->hosts[service->host_index];
        out->exposure_component +=
            bp_mul(host->criticality_bp, service->exposure_bp);
    }

    for (size_t i = 0u; i < sim->vulnerability_count; i += 1u) {
        const struct spg_sim_vulnerability *vuln = &sim->vulnerabilities[i];
        if (vuln->patched) {
            continue;
        }
        const struct spg_sim_service *service = &sim->services[vuln->service_index];
        const struct spg_sim_host    *host    = &sim->hosts[service->host_index];
        const uint64_t exposed =
            bp_mul(host->criticality_bp, service->exposure_bp);
        out->vulnerability_component +=
            (exposed * (uint64_t)vuln->severity_bp) / SPG_SIM_MAX_BASIS_POINTS;
    }

    for (size_t i = 0u; i < sim->credential_count; i += 1u) {
        const struct spg_sim_credential *cred = &sim->credentials[i];
        const struct spg_sim_account    *acct = &sim->accounts[cred->account_index];
        if (!acct->enabled) {
            continue;
        }
        const struct spg_sim_host *host = &sim->hosts[acct->host_index];
        const uint32_t weakness_bp = SPG_SIM_MAX_BASIS_POINTS - cred->strength_bp;
        out->credential_component +=
            bp_mul(host->criticality_bp, weakness_bp);
    }

    for (size_t i = 0u; i < sim->network_edge_count; i += 1u) {
        const struct spg_sim_network_edge *edge = &sim->network_edges[i];
        const struct spg_sim_host *from = &sim->hosts[edge->from_host_index];
        const struct spg_sim_host *to   = &sim->hosts[edge->to_host_index];
        const uint64_t pair = ((uint64_t)from->criticality_bp +
                               (uint64_t)to->criticality_bp) /
                              2u;
        out->reachability_component +=
            (pair * (uint64_t)edge->reachability_bp) /
            SPG_SIM_MAX_BASIS_POINTS;
    }

    out->total = out->asset_component + out->exposure_component +
                 out->vulnerability_component + out->credential_component +
                 out->reachability_component;
    return SPG_OK;
}
