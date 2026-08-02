#ifndef GEISTSHELL_SIM_EXECUTOR_H
#define GEISTSHELL_SIM_EXECUTOR_H

#include "geistshell/graph.h"
#include "geistshell/journal.h"
#include "geistshell/memory.h"
#include "geistshell/policy.h"
#include "geistshell/recommendation.h"
#include "geistshell/risk.h"
#include "geistshell/sim_config.h"
#include "geistshell/status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum spg_sim_exec_action {
    SPG_SIM_EXEC_NOOP = 0,
    SPG_SIM_EXEC_PATCH_VULNERABILITY,
    SPG_SIM_EXEC_DISABLE_ACCOUNT,
};

struct spg_sim_executor_state {
    struct spg_sim_config     *sim;
    struct spg_journal_writer *journal;
    struct spg_graph          *graph;
    struct spg_memory         *memory;
};

struct spg_sim_executor_config {
    uint32_t actor_id;
    uint64_t timestamp_ns;
    uint64_t parent_sequence;

    bool write_journal;
    bool update_graph;
    bool update_memory;
    bool has_recommendation_node;
    struct spg_node_id recommendation_node;
};

struct spg_sim_executor_workspace {
    size_t payload_capacity;
    char  *payload;
};

struct spg_sim_executor_result {
    enum spg_sim_exec_action action;
    size_t                   selected_index;
    bool                     mutated;

    struct spg_risk_score risk_before;
    struct spg_risk_score risk_after;

    uint64_t sim_sequence;
    uint64_t graph_sequence;
    uint64_t memory_sequence;

    bool               has_result_node;
    struct spg_node_id result_node;
    bool               has_result_edge;
    struct spg_edge_id result_edge;
    bool               has_memory_fact;
    struct spg_fact_id memory_fact;

    size_t payload_used;
    bool   payload_truncated;
};

[[nodiscard]] enum spg_status spg_sim_executor_step(
    struct spg_sim_executor_state *state,
    const struct spg_sim_executor_config *config,
    const struct spg_recommendation *recommendation,
    const struct spg_policy_decision *policy_decision,
    const struct spg_sim_executor_workspace *workspace,
    struct spg_sim_executor_result *result);

[[nodiscard]] const char *
spg_sim_exec_action_to_string(enum spg_sim_exec_action action);

#ifdef __cplusplus
}
#endif

#endif
