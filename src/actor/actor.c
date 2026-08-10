#include "geistshell/actor.h"

#include <string.h>

static bool workspace_valid(const struct spg_actor_step_workspace *workspace) {
    if (workspace == nullptr || workspace->context == nullptr ||
        workspace->context_capacity == 0u ||
        workspace->model_output == nullptr ||
        workspace->model_output_capacity == 0u) {
        return false;
    }
    if ((workspace->graph_ref_capacity > 0u &&
         workspace->graph_refs == nullptr) ||
        (workspace->memory_ref_capacity > 0u &&
         workspace->memory_refs == nullptr) ||
        (workspace->journal_ref_capacity > 0u &&
         workspace->journal_refs == nullptr)) {
        return false;
    }
    return true;
}

static enum spg_status
append_journal(struct spg_journal_writer *writer, const bool enabled,
               const uint64_t timestamp_ns, uint64_t *parent_sequence,
               const enum spg_journal_event_kind kind,
               const enum spg_status event_status, const size_t payload_n,
               const char payload[], uint64_t *out_sequence) {
    if (!enabled) {
        if (out_sequence != nullptr) {
            *out_sequence = 0u;
        }
        return SPG_OK;
    }
    if (writer == nullptr || parent_sequence == nullptr ||
        out_sequence == nullptr || (payload_n > 0u && payload == nullptr)) {
        return SPG_E_INVALID_ARG;
    }
    enum spg_status status = spg_journal_writer_append(
        writer, timestamp_ns, *parent_sequence, kind, event_status, payload_n,
        (const uint8_t *)payload, out_sequence);
    if (status == SPG_OK) {
        *parent_sequence = *out_sequence;
    }
    return status;
}

static enum spg_status add_graph_updates(struct spg_graph *graph,
                                         const uint32_t    actor_id,
                                         const size_t      context_n,
                                         const size_t      output_n,
                                         struct spg_actor_step_result *result) {
    if (graph == nullptr || result == nullptr) {
        return SPG_E_INVALID_ARG;
    }

    struct spg_node_id model_input = {};
    enum spg_status    status      = spg_graph_add_node(
        graph, SPG_GRAPH_NODE_OBSERVATION, actor_id,
        (struct spg_text_span){.offset = 0u, .length = context_n},
        &model_input);
    if (status != SPG_OK) {
        return status;
    }
    result->has_model_input_node = true;
    result->model_input_node     = model_input;

    struct spg_node_id recommendation = {};
    status                            = spg_graph_add_node(
        graph, SPG_GRAPH_NODE_PLAN, actor_id,
        (struct spg_text_span){.offset = 0u, .length = output_n},
        &recommendation);
    if (status != SPG_OK) {
        return status;
    }
    result->has_recommendation_node = true;
    result->recommendation_node     = recommendation;

    status =
        spg_graph_set_scores(graph, recommendation,
                             (struct spg_graph_scores){.confidence    = 0.5f,
                                                       .utility       = 0.5f,
                                                       .risk          = 0.0f,
                                                       .novelty       = 0.5f,
                                                       .cost_estimate = 0.0f});
    if (status != SPG_OK) {
        return status;
    }

    struct spg_edge_id edge = {};
    return spg_graph_add_edge(graph, SPG_GRAPH_EDGE_DERIVED_FROM,
                              recommendation, model_input, &edge);
}

static enum spg_status add_memory_update(struct spg_memory *memory,
                                         const uint64_t     source_event_id,
                                         const bool         has_graph_node,
                                         const struct spg_node_id graph_node,
                                         const size_t             output_n,
                                         struct spg_actor_step_result *result) {
    if (memory == nullptr || result == nullptr || source_event_id == 0u) {
        return SPG_E_INVALID_ARG;
    }
    struct spg_fact_id    fact   = {};
    const enum spg_status status = spg_memory_add_fact(
        memory, SPG_MEMORY_FACT_ARTIFACT, (struct spg_text_span){},
        (struct spg_text_span){},
        (struct spg_text_span){.offset = 0u, .length = output_n},
        source_event_id, has_graph_node, graph_node, 0.5f, &fact);
    if (status == SPG_OK) {
        result->has_recommendation_fact = true;
        result->recommendation_fact     = fact;
    }
    return status;
}

enum spg_status spg_actor_step(struct spg_actor_state                *state,
                               const struct spg_actor_step_config    *config,
                               const struct spg_actor_step_workspace *workspace,
                               struct spg_actor_step_result          *result) {
    if (state == nullptr || config == nullptr || !workspace_valid(workspace) ||
        result == nullptr || state->model == nullptr ||
        (config->write_journal && state->journal == nullptr) ||
        (config->update_graph && state->graph == nullptr) ||
        (config->update_memory && state->memory == nullptr)) {
        return SPG_E_INVALID_ARG;
    }
    *result                    = (struct spg_actor_step_result){};
    workspace->context[0]      = '\0';
    workspace->model_output[0] = '\0';

    struct spg_context_view context_view = {};
    spg_context_view_init(
        &context_view, workspace->graph_ref_capacity, workspace->graph_refs,
        workspace->memory_ref_capacity, workspace->memory_refs,
        workspace->journal_ref_capacity, workspace->journal_refs);

    const struct spg_context_sources context_sources = {
        .run                  = state->run,
        .usage                = state->usage,
        .graph                = state->graph,
        .memory               = state->memory,
        .journal_header_count = state->journal_header_count,
        .journal_headers      = state->journal_headers,
        .graph_text_n         = state->graph_text_n,
        .graph_text           = state->graph_text,
        .memory_text_n        = state->memory_text_n,
        .memory_text          = state->memory_text,
        .memory_index         = state->memory_index,
        .observation          = state->observation,
        .exemplars            = state->exemplars,
        .goal                 = state->goal,
        .directive            = state->directive,
        .machine              = state->machine,
        .machine_goal         = state->machine_goal,
    };

    enum spg_status status = spg_context_build(
        &context_sources, &config->context_limits, &context_view);
    if (status != SPG_OK) {
        return status;
    }
    result->context_graph_truncated   = context_view.graph_truncated;
    result->context_memory_truncated  = context_view.memory_truncated;
    result->context_journal_truncated = context_view.journal_truncated;

    status = spg_context_render(&context_sources, &context_view,
                                workspace->context_capacity, workspace->context,
                                &result->context_required);
    if (status != SPG_OK) {
        return status;
    }
    result->context_prompt_n = result->context_required - 1u;

    uint64_t parent_sequence = config->parent_sequence;
    status = append_journal(state->journal, config->write_journal,
                            config->timestamp_ns, &parent_sequence,
                            SPG_JOURNAL_EVENT_MODEL_INPUT, SPG_OK,
                            result->context_prompt_n, workspace->context,
                            &result->model_input_sequence);
    if (status != SPG_OK) {
        return status;
    }

    struct spg_model_generate_result model_result = {
        .output_capacity = workspace->model_output_capacity,
        .output          = workspace->model_output,
    };
    const struct spg_model_generate_request model_request = {
        .prompt_n          = result->context_prompt_n,
        .prompt            = workspace->context,
        .reset_session     = config->reset_model_session,
        .max_decode_tokens = config->max_decode_tokens,
    };
    status = spg_model_generate(state->model, &model_request, &model_result);
    result->model_output_n         = model_result.output_used;
    result->tokens_decoded         = model_result.tokens_decoded;
    result->model_output_truncated = model_result.output_truncated;
    result->stopped_by_token_limit = model_result.stopped_by_token_limit;

    const enum spg_status model_event_status = status;
    enum spg_status       journal_status     = append_journal(
        state->journal, config->write_journal, config->timestamp_ns,
        &parent_sequence, SPG_JOURNAL_EVENT_MODEL_OUTPUT, model_event_status,
        result->model_output_n, workspace->model_output,
        &result->model_output_sequence);
    if (journal_status != SPG_OK) {
        return journal_status;
    }
    if (status != SPG_OK) {
        return status;
    }

    if (config->update_graph) {
        status = add_graph_updates(state->graph, config->actor_id,
                                   result->context_prompt_n,
                                   result->model_output_n, result);
        if (status != SPG_OK) {
            return status;
        }
        journal_status = append_journal(
            state->journal, config->write_journal, config->timestamp_ns,
            &parent_sequence, SPG_JOURNAL_EVENT_GRAPH, status,
            result->model_output_n, workspace->model_output,
            &result->graph_sequence);
        if (journal_status != SPG_OK) {
            return journal_status;
        }
    }

    if (config->update_memory) {
        const uint64_t source_event_id = result->model_output_sequence != 0u
                                             ? result->model_output_sequence
                                             : UINT64_C(1);
        status                         = add_memory_update(
            state->memory, source_event_id, result->has_recommendation_node,
            result->recommendation_node, result->model_output_n, result);
        if (status != SPG_OK) {
            return status;
        }
        journal_status = append_journal(
            state->journal, config->write_journal, config->timestamp_ns,
            &parent_sequence, SPG_JOURNAL_EVENT_MEMORY, status,
            result->model_output_n, workspace->model_output,
            &result->memory_sequence);
        if (journal_status != SPG_OK) {
            return journal_status;
        }
    }

    return SPG_OK;
}
