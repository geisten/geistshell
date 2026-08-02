#include "geistshell/graph.h"

#include <math.h>
#include <stdio.h>

static int test_add_nodes_edges(void) {
    struct spg_graph   graph = {};
    struct spg_node_id a     = {};
    struct spg_node_id b     = {};
    struct spg_edge_id edge  = {};
    spg_graph_init(&graph);

    if (spg_graph_add_node(&graph, SPG_GRAPH_NODE_GOAL, 1u,
                           (struct spg_text_span){.offset = 0u, .length = 4u},
                           &a) != SPG_OK) {
        return 1;
    }
    if (spg_graph_add_node(&graph, SPG_GRAPH_NODE_PLAN, 2u,
                           (struct spg_text_span){.offset = 4u, .length = 4u},
                           &b) != SPG_OK) {
        return 1;
    }
    if (!spg_graph_node_valid(&graph, a) || !spg_graph_node_valid(&graph, b)) {
        return 1;
    }
    if (spg_graph_add_edge(&graph, SPG_GRAPH_EDGE_REFINES, a, b, &edge) !=
        SPG_OK) {
        return 1;
    }
    if (!spg_graph_edge_valid(&graph, edge) || graph.edge_count != 1u) {
        return 1;
    }
    return graph.nodes[a.index].flags == SPG_GRAPH_NODE_OPEN ? 0 : 1;
}

static int test_cycle_rejected(void) {
    struct spg_graph   graph = {};
    struct spg_node_id a     = {};
    struct spg_node_id b     = {};
    struct spg_node_id c     = {};
    struct spg_edge_id edge  = {};
    spg_graph_init(&graph);

    if (spg_graph_add_node(&graph, SPG_GRAPH_NODE_GOAL, 0u,
                           (struct spg_text_span){}, &a) != SPG_OK ||
        spg_graph_add_node(&graph, SPG_GRAPH_NODE_PLAN, 0u,
                           (struct spg_text_span){}, &b) != SPG_OK ||
        spg_graph_add_node(&graph, SPG_GRAPH_NODE_ACTION, 0u,
                           (struct spg_text_span){}, &c) != SPG_OK) {
        return 1;
    }
    if (spg_graph_add_edge(&graph, SPG_GRAPH_EDGE_REFINES, a, b, &edge) !=
            SPG_OK ||
        spg_graph_add_edge(&graph, SPG_GRAPH_EDGE_PROPOSES_ACTION, b, c,
                           &edge) != SPG_OK) {
        return 1;
    }
    return spg_graph_add_edge(&graph, SPG_GRAPH_EDGE_DERIVED_FROM, c, a,
                              &edge) == SPG_E_INVALID_STATE
               ? 0
               : 1;
}

static int test_self_edge_rejected(void) {
    struct spg_graph   graph = {};
    struct spg_node_id a     = {};
    struct spg_edge_id edge  = {};
    spg_graph_init(&graph);
    if (spg_graph_add_node(&graph, SPG_GRAPH_NODE_GOAL, 0u,
                           (struct spg_text_span){}, &a) != SPG_OK) {
        return 1;
    }
    return spg_graph_add_edge(&graph, SPG_GRAPH_EDGE_REFINES, a, a, &edge) ==
                   SPG_E_INVALID_STATE
               ? 0
               : 1;
}

static int test_stale_handle_rejected(void) {
    struct spg_graph   graph = {};
    struct spg_node_id a     = {};
    struct spg_node_id stale = {};
    struct spg_edge_id edge  = {};
    spg_graph_init(&graph);
    if (spg_graph_add_node(&graph, SPG_GRAPH_NODE_GOAL, 0u,
                           (struct spg_text_span){}, &a) != SPG_OK) {
        return 1;
    }
    stale = a;
    stale.generation += 1u;
    if (spg_graph_node_valid(&graph, stale)) {
        return 1;
    }
    return spg_graph_add_edge(&graph, SPG_GRAPH_EDGE_REFINES, a, stale,
                              &edge) == SPG_E_INVALID_ARG
               ? 0
               : 1;
}

static int test_scores_and_flags(void) {
    struct spg_graph   graph = {};
    struct spg_node_id node  = {};
    spg_graph_init(&graph);
    if (spg_graph_add_node(&graph, SPG_GRAPH_NODE_HYPOTHESIS, 0u,
                           (struct spg_text_span){}, &node) != SPG_OK) {
        return 1;
    }
    const struct spg_graph_scores scores = {
        .confidence    = 0.5f,
        .utility       = 1.0f,
        .risk          = 0.25f,
        .novelty       = 0.75f,
        .cost_estimate = 0.0f,
    };
    if (spg_graph_set_scores(&graph, node, scores) != SPG_OK) {
        return 1;
    }
    if (spg_graph_set_flags(&graph, node, SPG_GRAPH_NODE_CLOSED) != SPG_OK) {
        return 1;
    }
    if (graph.nodes[node.index].flags != SPG_GRAPH_NODE_CLOSED) {
        return 1;
    }
    const struct spg_graph_scores bad = {
        .confidence    = NAN,
        .utility       = 0.0f,
        .risk          = 0.0f,
        .novelty       = 0.0f,
        .cost_estimate = 0.0f,
    };
    if (spg_graph_set_scores(&graph, node, bad) != SPG_E_INVALID_ARG) {
        return 1;
    }
    return spg_graph_set_flags(&graph, node,
                               SPG_GRAPH_NODE_OPEN | SPG_GRAPH_NODE_CLOSED) ==
                   SPG_E_INVALID_ARG
               ? 0
               : 1;
}

static int test_node_capacity(void) {
    struct spg_graph   graph = {};
    struct spg_node_id node  = {};
    spg_graph_init(&graph);
    for (size_t i = 0u; i < SPG_GRAPH_MAX_NODES; i += 1u) {
        if (spg_graph_add_node(&graph, SPG_GRAPH_NODE_OBSERVATION, 0u,
                               (struct spg_text_span){}, &node) != SPG_OK) {
            return 1;
        }
    }
    return spg_graph_add_node(&graph, SPG_GRAPH_NODE_OBSERVATION, 0u,
                              (struct spg_text_span){}, &node) == SPG_E_LIMIT
               ? 0
               : 1;
}

static int test_edge_capacity(void) {
    struct spg_graph   graph = {};
    struct spg_node_id root  = {};
    struct spg_node_id node  = {};
    struct spg_edge_id edge  = {};
    spg_graph_init(&graph);
    if (spg_graph_add_node(&graph, SPG_GRAPH_NODE_GOAL, 0u,
                           (struct spg_text_span){}, &root) != SPG_OK) {
        return 1;
    }
    for (size_t i = 1u; i < SPG_GRAPH_MAX_NODES; i += 1u) {
        if (spg_graph_add_node(&graph, SPG_GRAPH_NODE_OBSERVATION, 0u,
                               (struct spg_text_span){}, &node) != SPG_OK) {
            return 1;
        }
        if (spg_graph_add_edge(&graph, SPG_GRAPH_EDGE_DERIVED_FROM, root, node,
                               &edge) != SPG_OK) {
            return 1;
        }
    }
    return graph.edge_count == SPG_GRAPH_MAX_NODES - 1u ? 0 : 1;
}

static int test_invalid_args(void) {
    struct spg_graph   graph = {};
    struct spg_node_id node  = {};
    spg_graph_init(&graph);
    if (spg_graph_add_node(nullptr, SPG_GRAPH_NODE_GOAL, 0u,
                           (struct spg_text_span){}, &node) !=
        SPG_E_INVALID_ARG) {
        return 1;
    }
    if (spg_graph_add_node(&graph, (enum spg_graph_node_kind)999, 0u,
                           (struct spg_text_span){}, &node) !=
        SPG_E_INVALID_ARG) {
        return 1;
    }
    return 0;
}

int main(void) {
    if (test_add_nodes_edges() != 0) {
        fprintf(stderr, "test_add_nodes_edges failed\n");
        return 1;
    }
    if (test_cycle_rejected() != 0) {
        fprintf(stderr, "test_cycle_rejected failed\n");
        return 1;
    }
    if (test_self_edge_rejected() != 0) {
        fprintf(stderr, "test_self_edge_rejected failed\n");
        return 1;
    }
    if (test_stale_handle_rejected() != 0) {
        fprintf(stderr, "test_stale_handle_rejected failed\n");
        return 1;
    }
    if (test_scores_and_flags() != 0) {
        fprintf(stderr, "test_scores_and_flags failed\n");
        return 1;
    }
    if (test_node_capacity() != 0) {
        fprintf(stderr, "test_node_capacity failed\n");
        return 1;
    }
    if (test_edge_capacity() != 0) {
        fprintf(stderr, "test_edge_capacity failed\n");
        return 1;
    }
    if (test_invalid_args() != 0) {
        fprintf(stderr, "test_invalid_args failed\n");
        return 1;
    }
    return 0;
}
