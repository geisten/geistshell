#ifndef GEISTSHELL_GRAPH_H
#define GEISTSHELL_GRAPH_H

#include "geistshell/sexpr.h"
#include "geistshell/status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPG_GRAPH_MAX_NODES 512u
#define SPG_GRAPH_MAX_EDGES 2048u

struct spg_node_id {
    uint32_t index;
    uint32_t generation;
};

struct spg_edge_id {
    uint32_t index;
    uint32_t generation;
};

enum spg_graph_node_kind {
    SPG_GRAPH_NODE_OBSERVATION = 0,
    SPG_GRAPH_NODE_HYPOTHESIS,
    SPG_GRAPH_NODE_GOAL,
    SPG_GRAPH_NODE_PLAN,
    SPG_GRAPH_NODE_ACTION,
    SPG_GRAPH_NODE_RESULT,
    SPG_GRAPH_NODE_MEMORY,
    SPG_GRAPH_NODE_POLICY_DECISION,
    SPG_GRAPH_NODE_EVAL_MARK,
};

enum spg_graph_edge_kind {
    SPG_GRAPH_EDGE_DERIVED_FROM = 0,
    SPG_GRAPH_EDGE_SUPPORTS,
    SPG_GRAPH_EDGE_CONTRADICTS,
    SPG_GRAPH_EDGE_REFINES,
    SPG_GRAPH_EDGE_PROPOSES_ACTION,
    SPG_GRAPH_EDGE_ACTION_RESULT,
    SPG_GRAPH_EDGE_BLOCKED_BY_POLICY,
    SPG_GRAPH_EDGE_UPDATES_MEMORY,
};

enum spg_graph_node_flags {
    SPG_GRAPH_NODE_OPEN    = 1u << 0u,
    SPG_GRAPH_NODE_CLOSED  = 1u << 1u,
    SPG_GRAPH_NODE_BLOCKED = 1u << 2u,
};

struct spg_graph_scores {
    float confidence;
    float utility;
    float risk;
    float novelty;
    float cost_estimate;
};

struct spg_graph_node {
    struct spg_node_id       id;
    enum spg_graph_node_kind kind;
    uint32_t                 flags;
    uint32_t                 actor_id;
    struct spg_text_span     payload;
    struct spg_graph_scores  scores;
    bool                     live;
};

struct spg_graph_edge {
    struct spg_edge_id       id;
    enum spg_graph_edge_kind kind;
    struct spg_node_id       from;
    struct spg_node_id       to;
    bool                     live;
};

struct spg_graph {
    size_t                node_count;
    size_t                edge_count;
    struct spg_graph_node nodes[SPG_GRAPH_MAX_NODES];
    struct spg_graph_edge edges[SPG_GRAPH_MAX_EDGES];
};

void spg_graph_init(struct spg_graph *graph);

[[nodiscard]] enum spg_status
spg_graph_add_node(struct spg_graph *graph, enum spg_graph_node_kind kind,
                   uint32_t actor_id, struct spg_text_span payload,
                   struct spg_node_id *out);

[[nodiscard]] enum spg_status
spg_graph_add_edge(struct spg_graph *graph, enum spg_graph_edge_kind kind,
                   struct spg_node_id from, struct spg_node_id to,
                   struct spg_edge_id *out);

[[nodiscard]] enum spg_status
spg_graph_set_scores(struct spg_graph *graph, struct spg_node_id node,
                     struct spg_graph_scores scores);

[[nodiscard]] enum spg_status
spg_graph_set_flags(struct spg_graph *graph, struct spg_node_id node,
                    uint32_t flags);

[[nodiscard]] bool spg_graph_node_valid(const struct spg_graph *graph,
                                        struct spg_node_id node);
[[nodiscard]] bool spg_graph_edge_valid(const struct spg_graph *graph,
                                        struct spg_edge_id edge);

#ifdef __cplusplus
}
#endif

#endif
