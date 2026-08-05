#include "geistshell/context.h"

#include <stdio.h>
#include <string.h>

static int test_budget_view(void) {
    const struct spg_run_config run = {
        .budgets = {
            .inference_steps = 10u,
            .tokens          = 100u,
            .shell_actions   = 2u,
        },
    };
    const struct spg_policy_usage usage = {
        .consumed = {
            .inference_steps = 3u,
            .tokens          = 125u,
            .shell_actions   = 1u,
        },
    };
    struct spg_context_graph_ref   graph_refs[1];
    struct spg_context_memory_ref  memory_refs[1];
    struct spg_context_journal_ref journal_refs[1];
    struct spg_context_view        view = {};
    spg_context_view_init(&view, 1u, graph_refs, 1u, memory_refs, 1u,
                          journal_refs);
    const struct spg_context_sources sources = {
        .run   = &run,
        .usage = &usage,
    };
    const struct spg_context_limits limits = {
        .graph_nodes    = 1u,
        .memory_facts   = 1u,
        .journal_events = 1u,
    };
    if (spg_context_build(&sources, &limits, &view) != SPG_OK) {
        return 1;
    }
    if (view.budgets.inference_steps.remaining != 7u ||
        view.budgets.tokens.remaining != 0u ||
        view.budgets.shell_actions.remaining != 1u) {
        return 1;
    }
    return 0;
}

static int test_graph_ranking_and_truncation(void) {
    const char text[] = "goal payload plan payload observation payload";
    struct spg_graph graph = {};
    struct spg_node_id observation = {};
    struct spg_node_id goal = {};
    struct spg_node_id plan = {};
    spg_graph_init(&graph);
    if (spg_graph_add_node(
            &graph, SPG_GRAPH_NODE_OBSERVATION, 1u,
            (struct spg_text_span){.offset = 23u, .length = 19u},
            &observation) != SPG_OK) {
        return 1;
    }
    if (spg_graph_add_node(
            &graph, SPG_GRAPH_NODE_GOAL, 1u,
            (struct spg_text_span){.offset = 0u, .length = 12u}, &goal) !=
        SPG_OK) {
        return 1;
    }
    if (spg_graph_add_node(
            &graph, SPG_GRAPH_NODE_PLAN, 1u,
            (struct spg_text_span){.offset = 13u, .length = 12u}, &plan) !=
        SPG_OK) {
        return 1;
    }
    if (spg_graph_set_scores(
            &graph, plan,
            (struct spg_graph_scores){.confidence = 0.9f,
                                      .utility = 1.0f,
                                      .risk = 0.0f,
                                      .novelty = 0.8f,
                                      .cost_estimate = 0.1f}) != SPG_OK) {
        return 1;
    }

    struct spg_context_graph_ref   graph_refs[2];
    struct spg_context_memory_ref  memory_refs[1];
    struct spg_context_journal_ref journal_refs[1];
    struct spg_context_view        view = {};
    spg_context_view_init(&view, 2u, graph_refs, 1u, memory_refs, 1u,
                          journal_refs);
    const struct spg_context_sources sources = {
        .graph        = &graph,
        .graph_text_n = sizeof text - 1u,
        .graph_text   = text,
    };
    const struct spg_context_limits limits = {
        .graph_nodes    = 2u,
        .memory_facts   = 0u,
        .journal_events = 0u,
    };
    if (spg_context_build(&sources, &limits, &view) != SPG_OK) {
        return 1;
    }
    if (view.graph_ref_count != 2u || !view.graph_truncated) {
        return 1;
    }
    if (view.graph_refs[0].node.index != plan.index ||
        view.graph_refs[1].node.index != goal.index) {
        return 1;
    }
    return 0;
}

static int test_memory_ranking_ignores_rejected(void) {
    const char text[] = "host-a role server old fact";
    struct spg_memory memory = {};
    struct spg_fact_id old_fact = {};
    struct spg_fact_id constraint = {};
    spg_memory_init(&memory);
    if (spg_memory_add_fact(
            &memory, SPG_MEMORY_FACT_OBSERVATION,
            (struct spg_text_span){.offset = 18u, .length = 8u},
            (struct spg_text_span){}, (struct spg_text_span){}, 1u, false,
            (struct spg_node_id){}, 1.0f, &old_fact) != SPG_OK) {
        return 1;
    }
    if (spg_memory_add_fact(
            &memory, SPG_MEMORY_FACT_CONSTRAINT,
            (struct spg_text_span){.offset = 0u, .length = 6u},
            (struct spg_text_span){.offset = 7u, .length = 4u},
            (struct spg_text_span){.offset = 12u, .length = 6u}, 4000u,
            false, (struct spg_node_id){}, 0.7f, &constraint) != SPG_OK) {
        return 1;
    }
    if (spg_memory_set_status(&memory, old_fact, SPG_MEMORY_FACT_REJECTED) !=
        SPG_OK) {
        return 1;
    }

    struct spg_context_graph_ref   graph_refs[1];
    struct spg_context_memory_ref  memory_refs[2];
    struct spg_context_journal_ref journal_refs[1];
    struct spg_context_view        view = {};
    spg_context_view_init(&view, 1u, graph_refs, 2u, memory_refs, 1u,
                          journal_refs);
    const struct spg_context_sources sources = {
        .memory        = &memory,
        .memory_text_n = sizeof text - 1u,
        .memory_text   = text,
    };
    const struct spg_context_limits limits = {
        .graph_nodes    = 0u,
        .memory_facts   = 2u,
        .journal_events = 0u,
    };
    if (spg_context_build(&sources, &limits, &view) != SPG_OK) {
        return 1;
    }
    if (view.memory_ref_count != 1u || view.memory_refs[0].fact.index !=
                                           constraint.index) {
        return 1;
    }
    return 0;
}

static int test_recent_events_are_chronological(void) {
    const struct spg_journal_record_header headers[4] = {
        {.sequence = 10u, .event_kind = SPG_JOURNAL_EVENT_RUN_START},
        {.sequence = 12u, .event_kind = SPG_JOURNAL_EVENT_ACTION},
        {.sequence = 11u, .event_kind = SPG_JOURNAL_EVENT_MODEL_OUTPUT},
        {.sequence = 13u, .event_kind = SPG_JOURNAL_EVENT_RESULT},
    };
    struct spg_context_graph_ref   graph_refs[1];
    struct spg_context_memory_ref  memory_refs[1];
    struct spg_context_journal_ref journal_refs[2];
    struct spg_context_view        view = {};
    spg_context_view_init(&view, 1u, graph_refs, 1u, memory_refs, 2u,
                          journal_refs);
    const struct spg_context_sources sources = {
        .journal_header_count = 4u,
        .journal_headers      = headers,
    };
    const struct spg_context_limits limits = {
        .graph_nodes    = 0u,
        .memory_facts   = 0u,
        .journal_events = 2u,
    };
    if (spg_context_build(&sources, &limits, &view) != SPG_OK) {
        return 1;
    }
    if (view.journal_ref_count != 2u || !view.journal_truncated) {
        return 1;
    }
    if (view.journal_refs[0].sequence != 12u ||
        view.journal_refs[1].sequence != 13u) {
        return 1;
    }
    return 0;
}

static int test_render_and_limit(void) {
    const char graph_text[]  = "deploy plan";
    const char memory_text[] = "host-a is isolated";
    struct spg_graph graph = {};
    struct spg_memory memory = {};
    struct spg_node_id node = {};
    struct spg_fact_id fact = {};
    spg_graph_init(&graph);
    spg_memory_init(&memory);
    if (spg_graph_add_node(
            &graph, SPG_GRAPH_NODE_PLAN, 9u,
            (struct spg_text_span){.offset = 0u, .length = 11u}, &node) !=
        SPG_OK) {
        return 1;
    }
    if (spg_memory_add_fact(
            &memory, SPG_MEMORY_FACT_CONSTRAINT,
            (struct spg_text_span){.offset = 0u, .length = 6u},
            (struct spg_text_span){.offset = 7u, .length = 2u},
            (struct spg_text_span){.offset = 10u, .length = 8u}, 2u, true,
            node, 0.8f, &fact) != SPG_OK) {
        return 1;
    }

    struct spg_context_graph_ref   graph_refs[1];
    struct spg_context_memory_ref  memory_refs[1];
    struct spg_context_journal_ref journal_refs[1];
    struct spg_context_view        view = {};
    spg_context_view_init(&view, 1u, graph_refs, 1u, memory_refs, 1u,
                          journal_refs);
    const struct spg_context_sources sources = {
        .graph         = &graph,
        .memory        = &memory,
        .graph_text_n  = sizeof graph_text - 1u,
        .graph_text    = graph_text,
        .memory_text_n = sizeof memory_text - 1u,
        .memory_text   = memory_text,
    };
    const struct spg_context_limits limits = {
        .graph_nodes    = 1u,
        .memory_facts   = 1u,
        .journal_events = 0u,
    };
    if (spg_context_build(&sources, &limits, &view) != SPG_OK) {
        return 1;
    }
    char small[16];
    size_t required = 0u;
    if (spg_context_render(&sources, &view, sizeof small, small, &required) !=
        SPG_E_LIMIT) {
        return 1;
    }
    if (required <= sizeof small || small[sizeof small - 1u] != '\0') {
        return 1;
    }
    char large[2048];
    if (spg_context_render(&sources, &view, sizeof large, large, &required) !=
        SPG_OK) {
        return 1;
    }
    if (strstr(large, "(graph") == nullptr ||
        strstr(large, "\"deploy plan\"") == nullptr ||
        strstr(large, "\"host-a\"") == nullptr) {
        return 1;
    }
    /* The contract advertises the memory actions to the model. */
    if (strstr(large, "memory_save") == nullptr ||
        strstr(large, "memory_read") == nullptr) {
        return 1;
    }
    return 0;
}

static int test_render_memory_index(void) {
    struct spg_context_graph_ref   graph_refs[1];
    struct spg_context_memory_ref  memory_refs[1];
    struct spg_context_journal_ref journal_refs[1];
    struct spg_context_view        view = {};
    spg_context_view_init(&view, 1u, graph_refs, 1u, memory_refs, 1u,
                          journal_refs);
    const struct spg_context_limits limits = {0};

    /* With an index, the rendered context carries a (memory_index ...) block. */
    const struct spg_context_sources with = {
        .memory_index = "- auth-flow: how login works\n",
    };
    if (spg_context_build(&with, &limits, &view) != SPG_OK) {
        return 1;
    }
    char   buf[2048];
    size_t required = 0u;
    if (spg_context_render(&with, &view, sizeof buf, buf, &required) != SPG_OK) {
        return 1;
    }
    /* Match the injected block (newline after the tag), not the contract's
     * explanatory mention of memory_index. */
    if (strstr(buf, "(memory_index\n") == nullptr ||
        strstr(buf, "- auth-flow: how login works") == nullptr) {
        return 1;
    }

    /* Without an index, the block is omitted. */
    const struct spg_context_sources without = {.memory_index = nullptr};
    if (spg_context_render(&without, &view, sizeof buf, buf, &required) !=
        SPG_OK) {
        return 1;
    }
    return strstr(buf, "(memory_index\n") == nullptr ? 0 : 1;
}

static int test_invalid_args(void) {
    struct spg_context_graph_ref   graph_refs[1];
    struct spg_context_memory_ref  memory_refs[1];
    struct spg_context_journal_ref journal_refs[1];
    struct spg_context_view        view = {};
    struct spg_context_sources     sources = {
        .journal_header_count = 1u,
        .journal_headers      = nullptr,
    };
    const struct spg_context_limits limits = {
        .graph_nodes    = 1u,
        .memory_facts   = 1u,
        .journal_events = 1u,
    };
    spg_context_view_init(&view, 1u, graph_refs, 1u, memory_refs, 1u,
                          journal_refs);
    if (spg_context_build(nullptr, &limits, &view) != SPG_E_INVALID_ARG) {
        return 1;
    }
    if (spg_context_build(&sources, &limits, &view) != SPG_E_INVALID_ARG) {
        return 1;
    }
    return 0;
}

/* #26/B: concrete exemplars render into an (examples ...) section after the
 * (contract ...) schema, so a small model can imitate the DSL. Absent = no
 * such section. */
static int test_exemplars_render(void) {
    const struct spg_run_config    run   = {.budgets = {.tokens = 100u}};
    const struct spg_policy_usage  usage = {};
    struct spg_context_graph_ref   graph_refs[1];
    struct spg_context_memory_ref  memory_refs[1];
    struct spg_context_journal_ref journal_refs[1];
    struct spg_context_view        view = {};
    spg_context_view_init(&view, 1u, graph_refs, 1u, memory_refs, 1u,
                          journal_refs);
    const struct spg_context_limits limits = {
        .graph_nodes = 1u, .memory_facts = 1u, .journal_events = 1u};

    struct spg_context_sources sources = {
        .run       = &run,
        .usage     = &usage,
        .exemplars = "  (recommend (kind finish) (reason \"EXEMPLAR-MARK\"))\n",
    };
    if (spg_context_build(&sources, &limits, &view) != SPG_OK) {
        return 1;
    }
    char   buf[4096];
    size_t req = 0u;
    if (spg_context_render(&sources, &view, sizeof buf, buf, &req) != SPG_OK) {
        return 1;
    }
    if (strstr(buf, "(examples") == nullptr ||
        strstr(buf, "EXEMPLAR-MARK") == nullptr) {
        return 1;
    }
    /* the examples section follows the contract schema */
    if (strstr(buf, "(contract") == nullptr ||
        strstr(buf, "(contract") > strstr(buf, "(examples")) {
        return 1;
    }
    /* absent -> no examples section */
    sources.exemplars = nullptr;
    (void)spg_context_render(&sources, &view, sizeof buf, buf, &req);
    if (strstr(buf, "(examples") != nullptr) {
        return 1;
    }
    return 0;
}

/* A free-text goal renders as (goal "...") near the top (after the contract),
 * so the model reads its objective. Absent -> no goal line. */
static int test_goal_render(void) {
    const struct spg_run_config    run   = {.budgets = {.tokens = 100u}};
    const struct spg_policy_usage  usage = {};
    struct spg_context_graph_ref   graph_refs[1];
    struct spg_context_memory_ref  memory_refs[1];
    struct spg_context_journal_ref journal_refs[1];
    struct spg_context_view        view = {};
    spg_context_view_init(&view, 1u, graph_refs, 1u, memory_refs, 1u,
                          journal_refs);
    const struct spg_context_limits limits = {
        .graph_nodes = 1u, .memory_facts = 1u, .journal_events = 1u};

    struct spg_context_sources sources = {
        .run = &run, .usage = &usage, .goal = "print GOAL-MARK to stdout"};
    if (spg_context_build(&sources, &limits, &view) != SPG_OK) {
        return 1;
    }
    char   buf[4096];
    size_t req = 0u;
    if (spg_context_render(&sources, &view, sizeof buf, buf, &req) != SPG_OK) {
        return 1;
    }
    if (strstr(buf, "(goal ") == nullptr ||
        strstr(buf, "GOAL-MARK") == nullptr) {
        return 1;
    }
    if (strstr(buf, "(contract") == nullptr ||
        strstr(buf, "(contract") > strstr(buf, "(goal ")) {
        return 1; /* goal follows the contract */
    }
    sources.goal = nullptr; /* absent -> no goal line */
    (void)spg_context_render(&sources, &view, sizeof buf, buf, &req);
    if (strstr(buf, "(goal ") != nullptr) {
        return 1;
    }

    /* A standing directive renders prominently as (directive "...") every step. */
    sources.directive = "emit finish once the goal output is produced";
    if (spg_context_render(&sources, &view, sizeof buf, buf, &req) != SPG_OK ||
        strstr(buf, "(directive ") == nullptr ||
        strstr(buf, "emit finish once") == nullptr) {
        return 1;
    }
    sources.directive = nullptr;
    (void)spg_context_render(&sources, &view, sizeof buf, buf, &req);
    if (strstr(buf, "(directive ") != nullptr) {
        return 1;
    }
    return 0;
}

int main(void) {
    if (test_exemplars_render() != 0) {
        fprintf(stderr, "test_exemplars_render failed\n");
        return 1;
    }
    if (test_goal_render() != 0) {
        fprintf(stderr, "test_goal_render failed\n");
        return 1;
    }
    if (test_budget_view() != 0) {
        fprintf(stderr, "test_budget_view failed\n");
        return 1;
    }
    if (test_graph_ranking_and_truncation() != 0) {
        fprintf(stderr, "test_graph_ranking_and_truncation failed\n");
        return 1;
    }
    if (test_memory_ranking_ignores_rejected() != 0) {
        fprintf(stderr, "test_memory_ranking_ignores_rejected failed\n");
        return 1;
    }
    if (test_recent_events_are_chronological() != 0) {
        fprintf(stderr, "test_recent_events_are_chronological failed\n");
        return 1;
    }
    if (test_render_and_limit() != 0) {
        fprintf(stderr, "test_render_and_limit failed\n");
        return 1;
    }
    if (test_render_memory_index() != 0) {
        fprintf(stderr, "test_render_memory_index failed\n");
        return 1;
    }
    if (test_invalid_args() != 0) {
        fprintf(stderr, "test_invalid_args failed\n");
        return 1;
    }
    return 0;
}
