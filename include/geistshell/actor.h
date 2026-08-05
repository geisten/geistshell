#ifndef GEISTSHELL_ACTOR_H
#define GEISTSHELL_ACTOR_H

#include "geistshell/context.h"
#include "geistshell/graph.h"
#include "geistshell/journal.h"
#include "geistshell/memory.h"
#include "geistshell/model_adapter.h"
#include "geistshell/policy.h"
#include "geistshell/run_config.h"
#include "geistshell/status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct spg_actor_state {
    struct spg_graph         *graph;
    struct spg_memory        *memory;
    struct spg_journal_writer *journal;
    struct spg_model_adapter *model;

    const struct spg_run_config   *run;
    const struct spg_policy_usage *usage;

    size_t                                  journal_header_count;
    const struct spg_journal_record_header *journal_headers;

    size_t      graph_text_n;
    const char *graph_text;
    size_t      memory_text_n;
    const char *memory_text;
    const char *memory_index;
    const char *observation;
    const char *exemplars;
    const char *goal;
    const char *directive;
};

struct spg_actor_step_config {
    uint32_t actor_id;
    uint64_t timestamp_ns;
    uint64_t parent_sequence;

    struct spg_context_limits context_limits;
    size_t                    max_decode_tokens;

    bool reset_model_session;
    bool write_journal;
    bool update_graph;
    bool update_memory;
};

struct spg_actor_step_workspace {
    size_t context_capacity;
    char  *context;

    size_t model_output_capacity;
    char  *model_output;

    size_t                        graph_ref_capacity;
    struct spg_context_graph_ref *graph_refs;

    size_t                         memory_ref_capacity;
    struct spg_context_memory_ref *memory_refs;

    size_t                          journal_ref_capacity;
    struct spg_context_journal_ref *journal_refs;
};

struct spg_actor_step_result {
    size_t context_required;
    size_t context_prompt_n;
    size_t model_output_n;
    size_t tokens_decoded;

    bool context_graph_truncated;
    bool context_memory_truncated;
    bool context_journal_truncated;
    bool model_output_truncated;
    bool stopped_by_token_limit;

    uint64_t model_input_sequence;
    uint64_t model_output_sequence;
    uint64_t graph_sequence;
    uint64_t memory_sequence;

    bool               has_model_input_node;
    struct spg_node_id model_input_node;
    bool               has_recommendation_node;
    struct spg_node_id recommendation_node;
    bool               has_recommendation_fact;
    struct spg_fact_id recommendation_fact;
};

[[nodiscard]] enum spg_status
spg_actor_step(struct spg_actor_state *state,
               const struct spg_actor_step_config *config,
               const struct spg_actor_step_workspace *workspace,
               struct spg_actor_step_result *result);

#ifdef __cplusplus
}
#endif

#endif
