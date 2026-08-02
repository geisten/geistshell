#ifndef GEISTSHELL_POLICY_GATE_H
#define GEISTSHELL_POLICY_GATE_H

#include "geistshell/graph.h"
#include "geistshell/journal.h"
#include "geistshell/policy.h"
#include "geistshell/policy_config.h"
#include "geistshell/recommendation.h"
#include "geistshell/status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct spg_policy_gate_state {
    size_t                          policy_text_n;
    const char                     *policy_text;
    size_t                          recommendation_text_n;
    const char                     *recommendation_text;
    const struct spg_policy_config *policy;
    const struct spg_policy_usage  *usage;

    struct spg_journal_writer *journal;
    struct spg_graph          *graph;
};

struct spg_policy_gate_config {
    uint32_t actor_id;
    uint64_t timestamp_ns;
    uint64_t parent_sequence;

    bool write_journal;
    bool update_graph_on_deny;
    bool has_recommendation_node;
    struct spg_node_id recommendation_node;
};

struct spg_policy_gate_workspace {
    size_t payload_capacity;
    char  *payload;
};

struct spg_policy_gate_result {
    struct spg_policy_decision decision;

    uint64_t policy_sequence;

    bool               has_policy_node;
    struct spg_node_id policy_node;
    bool               has_blocked_edge;
    struct spg_edge_id blocked_edge;

    size_t payload_used;
    bool   payload_truncated;
};

[[nodiscard]] enum spg_status
spg_policy_gate_step(const struct spg_policy_gate_state *state,
                     const struct spg_policy_gate_config *config,
                     const struct spg_recommendation *recommendation,
                     const struct spg_policy_gate_workspace *workspace,
                     struct spg_policy_gate_result *result);

#ifdef __cplusplus
}
#endif

#endif
