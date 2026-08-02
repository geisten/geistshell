#ifndef GEISTSHELL_POLICY_CONFIG_H
#define GEISTSHELL_POLICY_CONFIG_H

#include "geistshell/run_config.h"
#include "geistshell/sexpr.h"
#include "geistshell/status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPG_POLICY_MAX_CAPABILITIES 32u

enum spg_policy_network_default {
    SPG_POLICY_NETWORK_DENY = 0,
    SPG_POLICY_NETWORK_ALLOW,
};

enum spg_policy_capability_kind {
    SPG_POLICY_CAP_LOCAL_SHELL = 0,
    SPG_POLICY_CAP_SSH_AUTH_PROBE,
    SPG_POLICY_CAP_SIMULATOR,
    SPG_POLICY_CAP_MEMORY,
};

struct spg_policy_capability {
    struct spg_text_span          name;
    enum spg_policy_capability_kind kind;
    bool                         enabled;
    uint64_t                     budget;
};

struct spg_policy_config {
    enum spg_policy_network_default network_default;
    struct spg_run_budgets          budgets;

    size_t                       capability_count;
    struct spg_policy_capability capabilities[SPG_POLICY_MAX_CAPABILITIES];
};

struct spg_policy_config_error {
    enum spg_status status;
    uint32_t        node_index;
    size_t          offset;
};

[[nodiscard]] enum spg_status spg_policy_config_load(
    size_t input_n, const char input[], size_t token_capacity,
    struct spg_sexpr_token tokens[static token_capacity], size_t node_capacity,
    struct spg_sexpr_node nodes[static node_capacity],
    struct spg_policy_config *out, struct spg_policy_config_error *error);

#ifdef __cplusplus
}
#endif

#endif
