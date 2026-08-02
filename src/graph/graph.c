#include "geistshell/graph.h"

#include <math.h>
#include <string.h>

static bool node_kind_valid(const enum spg_graph_node_kind kind) {
    return kind == SPG_GRAPH_NODE_OBSERVATION ||
           kind == SPG_GRAPH_NODE_HYPOTHESIS ||
           kind == SPG_GRAPH_NODE_GOAL || kind == SPG_GRAPH_NODE_PLAN ||
           kind == SPG_GRAPH_NODE_ACTION || kind == SPG_GRAPH_NODE_RESULT ||
           kind == SPG_GRAPH_NODE_MEMORY ||
           kind == SPG_GRAPH_NODE_POLICY_DECISION ||
           kind == SPG_GRAPH_NODE_EVAL_MARK;
}

static bool edge_kind_valid(const enum spg_graph_edge_kind kind) {
    return kind == SPG_GRAPH_EDGE_DERIVED_FROM ||
           kind == SPG_GRAPH_EDGE_SUPPORTS ||
           kind == SPG_GRAPH_EDGE_CONTRADICTS ||
           kind == SPG_GRAPH_EDGE_REFINES ||
           kind == SPG_GRAPH_EDGE_PROPOSES_ACTION ||
           kind == SPG_GRAPH_EDGE_ACTION_RESULT ||
           kind == SPG_GRAPH_EDGE_BLOCKED_BY_POLICY ||
           kind == SPG_GRAPH_EDGE_UPDATES_MEMORY;
}

static bool score_valid(const float value) {
    return isfinite(value) && value >= 0.0f && value <= 1.0f;
}

static bool scores_valid(const struct spg_graph_scores scores) {
    return score_valid(scores.confidence) && score_valid(scores.utility) &&
           score_valid(scores.risk) && score_valid(scores.novelty) &&
           score_valid(scores.cost_estimate);
}

void spg_graph_init(struct spg_graph *graph) {
    if (graph == nullptr) {
        return;
    }
    *graph = (struct spg_graph){};
}

bool spg_graph_node_valid(const struct spg_graph *graph,
                          const struct spg_node_id node) {
    if (graph == nullptr || node.index >= graph->node_count) {
        return false;
    }
    const struct spg_graph_node *slot = &graph->nodes[node.index];
    return slot->live && slot->id.generation == node.generation;
}

bool spg_graph_edge_valid(const struct spg_graph *graph,
                          const struct spg_edge_id edge) {
    if (graph == nullptr || edge.index >= graph->edge_count) {
        return false;
    }
    const struct spg_graph_edge *slot = &graph->edges[edge.index];
    return slot->live && slot->id.generation == edge.generation;
}

enum spg_status spg_graph_add_node(struct spg_graph *graph,
                                   const enum spg_graph_node_kind kind,
                                   const uint32_t actor_id,
                                   const struct spg_text_span payload,
                                   struct spg_node_id *out) {
    if (graph == nullptr || out == nullptr || !node_kind_valid(kind)) {
        return SPG_E_INVALID_ARG;
    }
    if (graph->node_count >= SPG_GRAPH_MAX_NODES) {
        return SPG_E_LIMIT;
    }
    const uint32_t index = (uint32_t)graph->node_count;
    const struct spg_node_id id = {
        .index      = index,
        .generation = 1u,
    };
    graph->nodes[index] = (struct spg_graph_node){
        .id       = id,
        .kind     = kind,
        .flags    = SPG_GRAPH_NODE_OPEN,
        .actor_id = actor_id,
        .payload  = payload,
        .scores   = {},
        .live     = true,
    };
    graph->node_count += 1u;
    *out = id;
    return SPG_OK;
}

static bool reaches(const struct spg_graph *graph, const uint32_t start,
                    const uint32_t target) {
    bool     seen[SPG_GRAPH_MAX_NODES] = {};
    uint32_t stack[SPG_GRAPH_MAX_NODES];
    size_t   stack_n = 0u;

    stack[stack_n] = start;
    stack_n += 1u;

    while (stack_n > 0u) {
        stack_n -= 1u;
        const uint32_t node = stack[stack_n];
        if (node == target) {
            return true;
        }
        if (seen[node]) {
            continue;
        }
        seen[node] = true;
        for (size_t i = 0u; i < graph->edge_count; i += 1u) {
            const struct spg_graph_edge *edge = &graph->edges[i];
            if (!edge->live || edge->from.index != node) {
                continue;
            }
            if (edge->to.index >= graph->node_count || seen[edge->to.index]) {
                continue;
            }
            if (stack_n >= SPG_GRAPH_MAX_NODES) {
                return false;
            }
            stack[stack_n] = edge->to.index;
            stack_n += 1u;
        }
    }
    return false;
}

enum spg_status spg_graph_add_edge(struct spg_graph *graph,
                                   const enum spg_graph_edge_kind kind,
                                   const struct spg_node_id from,
                                   const struct spg_node_id to,
                                   struct spg_edge_id *out) {
    if (graph == nullptr || out == nullptr || !edge_kind_valid(kind)) {
        return SPG_E_INVALID_ARG;
    }
    if (!spg_graph_node_valid(graph, from) || !spg_graph_node_valid(graph, to)) {
        return SPG_E_INVALID_ARG;
    }
    if (from.index == to.index || reaches(graph, to.index, from.index)) {
        return SPG_E_INVALID_STATE;
    }
    if (graph->edge_count >= SPG_GRAPH_MAX_EDGES) {
        return SPG_E_LIMIT;
    }
    const uint32_t index = (uint32_t)graph->edge_count;
    const struct spg_edge_id id = {
        .index      = index,
        .generation = 1u,
    };
    graph->edges[index] = (struct spg_graph_edge){
        .id   = id,
        .kind = kind,
        .from = from,
        .to   = to,
        .live = true,
    };
    graph->edge_count += 1u;
    *out = id;
    return SPG_OK;
}

enum spg_status spg_graph_set_scores(struct spg_graph *graph,
                                     const struct spg_node_id node,
                                     const struct spg_graph_scores scores) {
    if (!spg_graph_node_valid(graph, node) || !scores_valid(scores)) {
        return SPG_E_INVALID_ARG;
    }
    graph->nodes[node.index].scores = scores;
    return SPG_OK;
}

enum spg_status spg_graph_set_flags(struct spg_graph *graph,
                                    const struct spg_node_id node,
                                    const uint32_t flags) {
    const uint32_t known = SPG_GRAPH_NODE_OPEN | SPG_GRAPH_NODE_CLOSED |
                           SPG_GRAPH_NODE_BLOCKED;
    if (!spg_graph_node_valid(graph, node) || (flags & ~known) != 0u) {
        return SPG_E_INVALID_ARG;
    }
    if ((flags & SPG_GRAPH_NODE_OPEN) != 0u &&
        (flags & SPG_GRAPH_NODE_CLOSED) != 0u) {
        return SPG_E_INVALID_ARG;
    }
    graph->nodes[node.index].flags = flags;
    return SPG_OK;
}
