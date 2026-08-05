#ifndef GEISTSHELL_CONTEXT_H
#define GEISTSHELL_CONTEXT_H

#include "geistshell/graph.h"
#include "geistshell/journal.h"
#include "geistshell/memory.h"
#include "geistshell/policy.h"
#include "geistshell/run_config.h"
#include "geistshell/status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct spg_context_limits {
    size_t graph_nodes;
    size_t memory_facts;
    size_t journal_events;
};

struct spg_context_sources {
    const struct spg_run_config       *run;
    const struct spg_policy_usage     *usage;
    const struct spg_graph            *graph;
    const struct spg_memory           *memory;
    size_t                             journal_header_count;
    const struct spg_journal_record_header *journal_headers;
    size_t                             graph_text_n;
    const char                        *graph_text;
    size_t                             memory_text_n;
    const char                        *memory_text;
    /* Pre-rendered long-term memory index (one hook per line), or null. */
    const char                        *memory_index;
    /* Content of the most recently recalled memory (memory_read), or null. */
    const char                        *observation;
    /* Optional concrete worked examples of the recommendation form, rendered
     * right after the (contract ...) schema. The schema is a grammar; a small
     * model imitates filled-in examples far more reliably than it parses a
     * grammar. Null = none (unchanged behaviour). */
    const char                        *exemplars;
    /* Optional free-text task, rendered near the top as (goal "..."). Null =
     * none: the scenario graph is the whole task. */
    const char                        *goal;
    /* Optional standing directive (a learned lesson) rendered prominently as
     * (directive "...") every step — a stronger channel than the mind-palace
     * index for steering a small model's behaviour (geistshell#40 follow-up).
     * Null = none. */
    const char                        *directive;
};

struct spg_context_budget_item {
    uint64_t configured;
    uint64_t consumed;
    uint64_t remaining;
};

struct spg_context_budget_view {
    struct spg_context_budget_item inference_steps;
    struct spg_context_budget_item tokens;
    struct spg_context_budget_item shell_actions;
    struct spg_context_budget_item sim_actions;
    struct spg_context_budget_item memory_actions;
    struct spg_context_budget_item wall_ms;
    struct spg_context_budget_item journal_bytes;
    struct spg_context_budget_item risk_bp;
};

struct spg_context_graph_ref {
    struct spg_node_id node;
    uint32_t           rank;
};

struct spg_context_memory_ref {
    struct spg_fact_id fact;
    uint32_t           rank;
};

struct spg_context_journal_ref {
    uint64_t sequence;
    uint64_t parent_sequence;
    uint64_t payload_bytes;
    uint32_t event_kind;
    uint32_t status;
    size_t   source_index;
};

struct spg_context_view {
    struct spg_context_budget_view budgets;

    size_t                        graph_ref_count;
    size_t                        graph_ref_capacity;
    struct spg_context_graph_ref *graph_refs;
    bool                          graph_truncated;

    size_t                         memory_ref_count;
    size_t                         memory_ref_capacity;
    struct spg_context_memory_ref *memory_refs;
    bool                           memory_truncated;

    size_t                          journal_ref_count;
    size_t                          journal_ref_capacity;
    struct spg_context_journal_ref *journal_refs;
    bool                            journal_truncated;
};

void spg_context_view_init(
    struct spg_context_view *view,
    size_t graph_ref_capacity,
    struct spg_context_graph_ref graph_refs[static graph_ref_capacity],
    size_t memory_ref_capacity,
    struct spg_context_memory_ref memory_refs[static memory_ref_capacity],
    size_t journal_ref_capacity,
    struct spg_context_journal_ref journal_refs[static journal_ref_capacity]);

[[nodiscard]] enum spg_status
spg_context_build(const struct spg_context_sources *sources,
                  const struct spg_context_limits *limits,
                  struct spg_context_view *view);

[[nodiscard]] enum spg_status spg_context_render(
    const struct spg_context_sources *sources,
    const struct spg_context_view *view, size_t dst_capacity,
    char dst[static dst_capacity], size_t *out_required);

#ifdef __cplusplus
}
#endif

#endif
