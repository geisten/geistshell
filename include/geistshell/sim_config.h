#ifndef GEISTSHELL_SIM_CONFIG_H
#define GEISTSHELL_SIM_CONFIG_H

#include "geistshell/sexpr.h"
#include "geistshell/status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPG_SIM_MAX_HOSTS 64u
#define SPG_SIM_MAX_SERVICES 128u
#define SPG_SIM_MAX_ACCOUNTS 128u
#define SPG_SIM_MAX_CREDENTIALS 128u
#define SPG_SIM_MAX_VULNERABILITIES 128u
#define SPG_SIM_MAX_NETWORK_EDGES 256u
#define SPG_SIM_MAX_BASIS_POINTS 10000u

struct spg_sim_host {
    struct spg_text_span id;
    uint32_t             criticality_bp;
};

struct spg_sim_service {
    struct spg_text_span id;
    uint32_t             host_index;
    struct spg_text_span name;
    uint32_t             port;
    uint32_t             exposure_bp;
};

struct spg_sim_account {
    struct spg_text_span id;
    uint32_t             host_index;
    struct spg_text_span username;
    bool                 enabled;
};

struct spg_sim_credential {
    struct spg_text_span id;
    uint32_t             account_index;
    uint32_t             strength_bp;
};

struct spg_sim_vulnerability {
    struct spg_text_span id;
    uint32_t             service_index;
    uint32_t             severity_bp;
    bool                 patched;
};

struct spg_sim_network_edge {
    uint32_t from_host_index;
    uint32_t to_host_index;
    uint32_t reachability_bp;
};

struct spg_sim_config {
    size_t              host_count;
    struct spg_sim_host hosts[SPG_SIM_MAX_HOSTS];

    size_t                 service_count;
    struct spg_sim_service services[SPG_SIM_MAX_SERVICES];

    size_t                 account_count;
    struct spg_sim_account accounts[SPG_SIM_MAX_ACCOUNTS];

    size_t                    credential_count;
    struct spg_sim_credential credentials[SPG_SIM_MAX_CREDENTIALS];

    size_t                       vulnerability_count;
    struct spg_sim_vulnerability vulnerabilities[SPG_SIM_MAX_VULNERABILITIES];

    size_t                      network_edge_count;
    struct spg_sim_network_edge network_edges[SPG_SIM_MAX_NETWORK_EDGES];
};

struct spg_sim_config_error {
    enum spg_status status;
    uint32_t        node_index;
    size_t          offset;
};

[[nodiscard]] enum spg_status spg_sim_config_load(
    size_t input_n, const char input[], size_t token_capacity,
    struct spg_sexpr_token tokens[static token_capacity], size_t node_capacity,
    struct spg_sexpr_node nodes[static node_capacity],
    struct spg_sim_config *out, struct spg_sim_config_error *error);

#ifdef __cplusplus
}
#endif

#endif
