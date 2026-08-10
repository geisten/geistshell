#include "geistshell/policy_gate.h"

#include <string.h>

static bool workspace_valid(const struct spg_policy_gate_workspace *workspace) {
    return workspace != nullptr && workspace->payload != nullptr &&
           workspace->payload_capacity > 0u;
}

static bool span_eq_texts(const size_t left_n, const char left[],
                          const struct spg_text_span left_span,
                          const size_t right_n, const char right[],
                          const struct spg_text_span right_span) {
    if (left == nullptr || right == nullptr ||
        !spg_sexpr_span_valid(left_n, left_span) ||
        !spg_sexpr_span_valid(right_n, right_span) ||
        left_span.length != right_span.length) {
        return false;
    }
    return memcmp(left + left_span.offset, right + right_span.offset,
                  left_span.length) == 0;
}

static bool
resolve_capability_span(const struct spg_policy_gate_state *state,
                        const struct spg_recommendation    *recommendation,
                        struct spg_text_span               *out_policy_span) {
    if (state == nullptr || recommendation == nullptr ||
        out_policy_span == nullptr || state->policy == nullptr) {
        return false;
    }
    for (size_t i = 0u; i < state->policy->capability_count; i += 1u) {
        const struct spg_text_span candidate =
            state->policy->capabilities[i].name;
        if (span_eq_texts(state->recommendation_text_n,
                          state->recommendation_text,
                          recommendation->capability, state->policy_text_n,
                          state->policy_text, candidate)) {
            *out_policy_span = candidate;
            return true;
        }
    }
    return false;
}

static const char *
decision_kind_name(const enum spg_policy_decision_kind kind) {
    switch (kind) {
    case SPG_POLICY_DECISION_ALLOW:
        return "allow";
    case SPG_POLICY_DECISION_DENY:
        return "deny";
    }
    return "unknown";
}

static enum spg_status
render_payload(const struct spg_recommendation        *recommendation,
               const struct spg_policy_decision       *decision,
               const struct spg_policy_gate_workspace *workspace,
               struct spg_policy_gate_result          *result) {
    struct spg_sexpr_writer writer;
    spg_sexpr_writer_init(&writer, workspace->payload_capacity,
                          workspace->payload);

    enum spg_status status =
        spg_sexpr_writer_append_text(&writer, "(policy_decision (decision ");
    if (status == SPG_OK) {
        status = spg_sexpr_writer_append_text(
            &writer, decision_kind_name(decision->kind));
    }
    if (status == SPG_OK) {
        status = spg_sexpr_writer_append_text(&writer, ") (deny_reason ");
    }
    if (status == SPG_OK) {
        status = spg_sexpr_writer_append_text(
            &writer, spg_policy_deny_reason_to_string(decision->deny_reason));
    }
    if (status == SPG_OK) {
        status = spg_sexpr_writer_append_text(&writer, ") (action_kind ");
    }
    if (status == SPG_OK) {
        status = spg_sexpr_writer_append_text(
            &writer, spg_action_kind_to_string(recommendation->action_kind));
    }
    if (status == SPG_OK) {
        status = spg_sexpr_writer_append_text(&writer, ") (cost ");
    }
    if (status == SPG_OK) {
        status =
            spg_sexpr_writer_append_u64(&writer, recommendation->action.cost);
    }
    if (status == SPG_OK) {
        status = spg_sexpr_writer_append_text(&writer, ") (uses_network ");
    }
    if (status == SPG_OK) {
        status = spg_sexpr_writer_append_text(
            &writer, recommendation->action.uses_network ? "true" : "false");
    }
    if (status == SPG_OK) {
        status = spg_sexpr_writer_append_text(&writer, ") (confidence_bp ");
    }
    if (status == SPG_OK) {
        status =
            spg_sexpr_writer_append_u64(&writer, recommendation->confidence_bp);
    }
    if (status == SPG_OK) {
        status = spg_sexpr_writer_append_text(&writer, ") (capability_span ");
    }
    if (status == SPG_OK) {
        status = spg_sexpr_writer_append_size(
            &writer, recommendation->capability.offset);
    }
    if (status == SPG_OK) {
        status = spg_sexpr_writer_append_text(&writer, ":");
    }
    if (status == SPG_OK) {
        status = spg_sexpr_writer_append_size(
            &writer, recommendation->capability.length);
    }
    if (status == SPG_OK) {
        status = spg_sexpr_writer_append_text(&writer, "))");
    }

    result->payload_used      = writer.used;
    result->payload_truncated = writer.truncated;
    return status;
}

static enum spg_status journal_decision(
    struct spg_journal_writer *journal, const uint64_t timestamp_ns,
    const uint64_t parent_sequence, const struct spg_policy_decision *decision,
    const struct spg_policy_gate_workspace *workspace,
    const struct spg_policy_gate_result *result, uint64_t *out_sequence) {
    if (journal == nullptr || decision == nullptr || workspace == nullptr ||
        result == nullptr || out_sequence == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    const enum spg_status event_status =
        decision->kind == SPG_POLICY_DECISION_ALLOW ? SPG_OK
                                                    : SPG_E_POLICY_DENIED;
    return spg_journal_writer_append(
        journal, timestamp_ns, parent_sequence,
        SPG_JOURNAL_EVENT_POLICY_DECISION, event_status, result->payload_used,
        (const uint8_t *)workspace->payload, out_sequence);
}

static enum spg_status
add_deny_graph(struct spg_graph                    *graph,
               const struct spg_policy_gate_config *config,
               struct spg_policy_gate_result       *result) {
    if (graph == nullptr || config == nullptr || result == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    struct spg_node_id policy_node = {};
    enum spg_status    status      = spg_graph_add_node(
        graph, SPG_GRAPH_NODE_POLICY_DECISION, config->actor_id,
        (struct spg_text_span){.offset = 0u, .length = result->payload_used},
        &policy_node);
    if (status != SPG_OK) {
        return status;
    }
    result->has_policy_node = true;
    result->policy_node     = policy_node;

    status = spg_graph_set_flags(graph, policy_node, SPG_GRAPH_NODE_BLOCKED);
    if (status != SPG_OK) {
        return status;
    }
    status =
        spg_graph_set_scores(graph, policy_node,
                             (struct spg_graph_scores){.confidence    = 1.0f,
                                                       .utility       = 0.0f,
                                                       .risk          = 0.0f,
                                                       .novelty       = 0.0f,
                                                       .cost_estimate = 0.0f});
    if (status != SPG_OK) {
        return status;
    }

    if (config->has_recommendation_node) {
        if (!spg_graph_node_valid(graph, config->recommendation_node)) {
            return SPG_E_INVALID_ARG;
        }
        struct spg_edge_id edge = {};
        status =
            spg_graph_add_edge(graph, SPG_GRAPH_EDGE_BLOCKED_BY_POLICY,
                               config->recommendation_node, policy_node, &edge);
        if (status != SPG_OK) {
            return status;
        }
        result->has_blocked_edge = true;
        result->blocked_edge     = edge;
    }
    return SPG_OK;
}

/* Machine actions carry a target: a profile id, never a pid. Resolve it into
 * the span of text the recommendation used. */
static bool target_text(const struct spg_policy_gate_state *state,
                        const struct spg_recommendation    *rec,
                        char target[static SPG_PROCESS_ID_CAP]) {
    const struct spg_text_span span = rec->target;
    if (span.length == 0u || span.length + 1u > SPG_PROCESS_ID_CAP ||
        span.offset + span.length > state->recommendation_text_n) {
        return false;
    }
    memcpy(target, state->recommendation_text + span.offset, span.length);
    target[span.length] = '\0';
    return true;
}

/* Decide a machine action on process semantics alone; budget and capability
 * are still the general path's job.
 *
 * Order matters for what the journal ends up saying. "unmanaged" and
 * "protected" are different findings: the first means the operator never
 * declared this process, the second means they declared it off limits. */
static enum spg_policy_deny_reason
machine_denial(const struct spg_policy_gate_state *state,
               const struct spg_recommendation    *rec) {
    if (rec->action_kind != SPG_ACTION_MACHINE_PAUSE &&
        rec->action_kind != SPG_ACTION_MACHINE_RESUME) {
        return SPG_POLICY_DENY_NONE;
    }
    if (state->profile == nullptr || state->machine == nullptr) {
        /* No profile configured means nothing is managed, and nothing
         * unmanaged is actionable. */
        return SPG_POLICY_DENY_UNMANAGED_PROCESS;
    }
    char target[SPG_PROCESS_ID_CAP];
    if (!target_text(state, rec, target)) {
        return SPG_POLICY_DENY_INVALID_REQUEST;
    }

    const struct spg_process_profile_entry *entry = nullptr;
    for (size_t i = 0u; i < state->profile->count; i += 1u) {
        if (strcmp(state->profile->entries[i].id, target) == 0) {
            entry = &state->profile->entries[i];
            break;
        }
    }
    if (entry == nullptr) {
        return SPG_POLICY_DENY_UNMANAGED_PROCESS;
    }

    /* Pausing needs permission. Resuming does not: it can only restore the
     * state the machine had before we touched it, and denying it would leave a
     * process we stopped stopped. */
    if (rec->action_kind == SPG_ACTION_MACHINE_PAUSE && !entry->may_pause) {
        return SPG_POLICY_DENY_PROCESS_PROTECTED;
    }

    /* The target must be a process we actually observed this tick. Acting on
     * something that is not in the snapshot means acting on a guess. */
    for (size_t i = 0u; i < state->machine->n_processes; i += 1u) {
        if (strcmp(state->machine->processes[i].profile_id, target) == 0) {
            return SPG_POLICY_DENY_NONE;
        }
    }
    return SPG_POLICY_DENY_PROCESS_IDENTITY;
}

enum spg_status
spg_policy_gate_step(const struct spg_policy_gate_state     *state,
                     const struct spg_policy_gate_config    *config,
                     const struct spg_recommendation        *recommendation,
                     const struct spg_policy_gate_workspace *workspace,
                     struct spg_policy_gate_result          *result) {
    if (state == nullptr || config == nullptr || recommendation == nullptr ||
        !workspace_valid(workspace) || result == nullptr ||
        state->policy_text == nullptr ||
        state->recommendation_text == nullptr || state->policy == nullptr ||
        state->usage == nullptr || state->policy_text_n == 0u ||
        state->recommendation_text_n == 0u ||
        recommendation->state != SPG_RECOMMENDATION_VALID ||
        (config->write_journal && state->journal == nullptr) ||
        (config->update_graph_on_deny && state->graph == nullptr)) {
        return SPG_E_INVALID_ARG;
    }

    *result               = (struct spg_policy_gate_result){};
    workspace->payload[0] = '\0';

    struct spg_action_request         request = recommendation->action;
    const enum spg_policy_deny_reason machine_reason =
        machine_denial(state, recommendation);
    if (machine_reason != SPG_POLICY_DENY_NONE) {
        /* Decided before capability and budget on purpose: a process the
         * operator protected must not consume budget to be refused, and the
         * journal should say why it was refused, not that we ran out. */
        result->decision = (struct spg_policy_decision){
            .kind             = SPG_POLICY_DECISION_DENY,
            .deny_reason      = machine_reason,
            .capability_index = SIZE_MAX,
        };
    } else if (!resolve_capability_span(state, recommendation,
                                        &request.capability)) {
        result->decision = (struct spg_policy_decision){
            .kind             = SPG_POLICY_DECISION_DENY,
            .deny_reason      = SPG_POLICY_DENY_UNKNOWN_CAPABILITY,
            .capability_index = SIZE_MAX,
        };
    } else {
        enum spg_status status = spg_policy_decide(
            state->policy_text_n, state->policy_text, state->policy,
            state->usage, &request, &result->decision);
        if (status != SPG_OK) {
            return status;
        }
    }

    enum spg_status status =
        render_payload(recommendation, &result->decision, workspace, result);
    if (status != SPG_OK) {
        return status;
    }

    if (config->write_journal) {
        status = journal_decision(state->journal, config->timestamp_ns,
                                  config->parent_sequence, &result->decision,
                                  workspace, result, &result->policy_sequence);
        if (status != SPG_OK) {
            return status;
        }
    }

    if (config->update_graph_on_deny &&
        result->decision.kind == SPG_POLICY_DECISION_DENY) {
        status = add_deny_graph(state->graph, config, result);
        if (status != SPG_OK) {
            return status;
        }
    }

    return SPG_OK;
}
