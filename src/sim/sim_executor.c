#include "geistshell/sim_executor.h"

static uint64_t bp_mul(const uint32_t a, const uint32_t b) {
    return ((uint64_t)a * (uint64_t)b) / SPG_SIM_MAX_BASIS_POINTS;
}

const char *spg_sim_exec_action_to_string(
    const enum spg_sim_exec_action action) {
    switch (action) {
    case SPG_SIM_EXEC_NOOP:
        return "noop";
    case SPG_SIM_EXEC_PATCH_VULNERABILITY:
        return "patch_vulnerability";
    case SPG_SIM_EXEC_DISABLE_ACCOUNT:
        return "disable_account";
    }
    return "unknown";
}

static bool workspace_valid(const struct spg_sim_executor_workspace *workspace) {
    return workspace != nullptr && workspace->payload != nullptr &&
           workspace->payload_capacity > 0u;
}

static uint64_t vulnerability_contribution(const struct spg_sim_config *sim,
                                           const size_t index) {
    const struct spg_sim_vulnerability *vuln = &sim->vulnerabilities[index];
    if (vuln->patched) {
        return 0u;
    }
    const struct spg_sim_service *service = &sim->services[vuln->service_index];
    const struct spg_sim_host    *host    = &sim->hosts[service->host_index];
    const uint64_t exposed = bp_mul(host->criticality_bp, service->exposure_bp);
    return (exposed * (uint64_t)vuln->severity_bp) /
           SPG_SIM_MAX_BASIS_POINTS;
}

static uint64_t account_contribution(const struct spg_sim_config *sim,
                                     const size_t account_index) {
    const struct spg_sim_account *account = &sim->accounts[account_index];
    if (!account->enabled) {
        return 0u;
    }
    uint64_t worst = 0u;
    for (size_t i = 0u; i < sim->credential_count; i += 1u) {
        const struct spg_sim_credential *cred = &sim->credentials[i];
        if (cred->account_index != account_index) {
            continue;
        }
        const struct spg_sim_host *host = &sim->hosts[account->host_index];
        const uint32_t weakness_bp =
            SPG_SIM_MAX_BASIS_POINTS - cred->strength_bp;
        const uint64_t contribution = bp_mul(host->criticality_bp, weakness_bp);
        if (contribution > worst) {
            worst = contribution;
        }
    }
    return worst;
}

static void mutate_sim(struct spg_sim_config *sim,
                       struct spg_sim_executor_result *result) {
    result->action         = SPG_SIM_EXEC_NOOP;
    result->selected_index = SIZE_MAX;
    result->mutated        = false;

    uint64_t best = 0u;
    for (size_t i = 0u; i < sim->vulnerability_count; i += 1u) {
        const uint64_t contribution = vulnerability_contribution(sim, i);
        if (contribution > best) {
            best                   = contribution;
            result->selected_index = i;
            result->action         = SPG_SIM_EXEC_PATCH_VULNERABILITY;
        }
    }
    if (result->action == SPG_SIM_EXEC_PATCH_VULNERABILITY) {
        sim->vulnerabilities[result->selected_index].patched = true;
        result->mutated = true;
        return;
    }

    for (size_t i = 0u; i < sim->account_count; i += 1u) {
        const uint64_t contribution = account_contribution(sim, i);
        if (contribution > best) {
            best                   = contribution;
            result->selected_index = i;
            result->action         = SPG_SIM_EXEC_DISABLE_ACCOUNT;
        }
    }
    if (result->action == SPG_SIM_EXEC_DISABLE_ACCOUNT) {
        sim->accounts[result->selected_index].enabled = false;
        result->mutated = true;
    }
}

static enum spg_status render_payload(
    const struct spg_sim_executor_workspace *workspace,
    struct spg_sim_executor_result *result) {
    struct spg_sexpr_writer writer;
    spg_sexpr_writer_init(&writer, workspace->payload_capacity,
                          workspace->payload);

    enum spg_status status =
        spg_sexpr_writer_append_text(&writer, "(sim_result (action ");
    if (status == SPG_OK) {
        status = spg_sexpr_writer_append_text(
            &writer, spg_sim_exec_action_to_string(result->action));
    }
    if (status == SPG_OK) {
        status = spg_sexpr_writer_append_text(&writer, ") (selected_index ");
    }
    if (status == SPG_OK) {
        status = result->selected_index == SIZE_MAX
                     ? spg_sexpr_writer_append_text(&writer, "none")
                     : spg_sexpr_writer_append_size(&writer,
                                                    result->selected_index);
    }
    if (status == SPG_OK) {
        status = spg_sexpr_writer_append_text(&writer, ") (mutated ");
    }
    if (status == SPG_OK) {
        status = spg_sexpr_writer_append_text(
            &writer, result->mutated ? "true" : "false");
    }
    if (status == SPG_OK) {
        status = spg_sexpr_writer_append_text(&writer, ") (risk_before ");
    }
    if (status == SPG_OK) {
        status =
            spg_sexpr_writer_append_u64(&writer, result->risk_before.total);
    }
    if (status == SPG_OK) {
        status = spg_sexpr_writer_append_text(&writer, ") (risk_after ");
    }
    if (status == SPG_OK) {
        status = spg_sexpr_writer_append_u64(&writer, result->risk_after.total);
    }
    if (status == SPG_OK) {
        status = spg_sexpr_writer_append_text(&writer, "))");
    }

    result->payload_used      = writer.used;
    result->payload_truncated = writer.truncated;
    return status;
}

static enum spg_status journal_sim(
    struct spg_journal_writer *journal, const uint64_t timestamp_ns,
    const uint64_t parent_sequence,
    const struct spg_sim_executor_workspace *workspace,
    const struct spg_sim_executor_result *result, uint64_t *out_sequence) {
    if (journal == nullptr || workspace == nullptr || result == nullptr ||
        out_sequence == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    return spg_journal_writer_append(
        journal, timestamp_ns, parent_sequence, SPG_JOURNAL_EVENT_SIM, SPG_OK,
        result->payload_used, (const uint8_t *)workspace->payload,
        out_sequence);
}

static enum spg_status add_graph_result(
    struct spg_graph *graph, const struct spg_sim_executor_config *config,
    struct spg_sim_executor_result *result) {
    if (graph == nullptr || config == nullptr || result == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    struct spg_node_id node = {};
    enum spg_status status = spg_graph_add_node(
        graph, SPG_GRAPH_NODE_RESULT, config->actor_id,
        (struct spg_text_span){.offset = 0u,
                               .length = result->payload_used},
        &node);
    if (status != SPG_OK) {
        return status;
    }
    result->has_result_node = true;
    result->result_node     = node;

    status = spg_graph_set_scores(
        graph, node,
        (struct spg_graph_scores){.confidence    = 1.0f,
                                  .utility       = result->mutated ? 0.75f
                                                                   : 0.0f,
                                  .risk          = 0.0f,
                                  .novelty       = 0.25f,
                                  .cost_estimate = 0.0f});
    if (status != SPG_OK) {
        return status;
    }

    if (config->has_recommendation_node) {
        if (!spg_graph_node_valid(graph, config->recommendation_node)) {
            return SPG_E_INVALID_ARG;
        }
        struct spg_edge_id edge = {};
        status = spg_graph_add_edge(graph, SPG_GRAPH_EDGE_ACTION_RESULT,
                                    config->recommendation_node, node, &edge);
        if (status != SPG_OK) {
            return status;
        }
        result->has_result_edge = true;
        result->result_edge     = edge;
    }
    return SPG_OK;
}

static enum spg_status add_memory_fact(
    struct spg_memory *memory, const struct spg_sim_executor_result *result,
    const uint64_t source_event_id) {
    if (memory == nullptr || result == nullptr || source_event_id == 0u) {
        return SPG_E_INVALID_ARG;
    }
    struct spg_fact_id fact = {};
    const enum spg_status status = spg_memory_add_fact(
        memory, SPG_MEMORY_FACT_ARTIFACT, (struct spg_text_span){},
        (struct spg_text_span){},
        (struct spg_text_span){.offset = 0u, .length = result->payload_used},
        source_event_id, result->has_result_node, result->result_node, 1.0f,
        &fact);
    if (status != SPG_OK) {
        return status;
    }
    return SPG_OK;
}

enum spg_status spg_sim_executor_step(
    struct spg_sim_executor_state *state,
    const struct spg_sim_executor_config *config,
    const struct spg_recommendation *recommendation,
    const struct spg_policy_decision *policy_decision,
    const struct spg_sim_executor_workspace *workspace,
    struct spg_sim_executor_result *result) {
    if (state == nullptr || config == nullptr || recommendation == nullptr ||
        policy_decision == nullptr || !workspace_valid(workspace) ||
        result == nullptr || state->sim == nullptr ||
        recommendation->state != SPG_RECOMMENDATION_VALID ||
        recommendation->action_kind != SPG_ACTION_SIMULATOR ||
        policy_decision->kind != SPG_POLICY_DECISION_ALLOW ||
        (config->write_journal && state->journal == nullptr) ||
        (config->update_graph && state->graph == nullptr) ||
        (config->update_memory && state->memory == nullptr)) {
        return SPG_E_INVALID_ARG;
    }

    *result = (struct spg_sim_executor_result){
        .action         = SPG_SIM_EXEC_NOOP,
        .selected_index = SIZE_MAX,
    };
    workspace->payload[0] = '\0';

    enum spg_status status =
        spg_risk_evaluate(state->sim, &result->risk_before);
    if (status != SPG_OK) {
        return status;
    }
    struct spg_sim_config next_sim = *state->sim;
    mutate_sim(&next_sim, result);
    status = spg_risk_evaluate(&next_sim, &result->risk_after);
    if (status != SPG_OK) {
        return status;
    }
    status = render_payload(workspace, result);
    if (status != SPG_OK) {
        return status;
    }
    *state->sim = next_sim;

    if (config->write_journal) {
        status = journal_sim(state->journal, config->timestamp_ns,
                             config->parent_sequence, workspace, result,
                             &result->sim_sequence);
        if (status != SPG_OK) {
            return status;
        }
    }

    uint64_t parent_sequence = result->sim_sequence != 0u
                                   ? result->sim_sequence
                                   : config->parent_sequence;
    if (config->update_graph) {
        status = add_graph_result(state->graph, config, result);
        if (status != SPG_OK) {
            return status;
        }
        if (config->write_journal) {
            status = spg_journal_writer_append(
                state->journal, config->timestamp_ns, parent_sequence,
                SPG_JOURNAL_EVENT_GRAPH, SPG_OK, result->payload_used,
                (const uint8_t *)workspace->payload, &result->graph_sequence);
            if (status != SPG_OK) {
                return status;
            }
            parent_sequence = result->graph_sequence;
        }
    }

    if (config->update_memory) {
        const uint64_t source_event_id =
            result->sim_sequence != 0u ? result->sim_sequence : UINT64_C(1);
        status = add_memory_fact(state->memory, result, source_event_id);
        if (status != SPG_OK) {
            return status;
        }
        result->has_memory_fact = state->memory->fact_count > 0u;
        if (result->has_memory_fact) {
            result->memory_fact = state->memory->facts[state->memory->fact_count - 1u].id;
        }
        if (config->write_journal) {
            status = spg_journal_writer_append(
                state->journal, config->timestamp_ns, parent_sequence,
                SPG_JOURNAL_EVENT_MEMORY, SPG_OK, result->payload_used,
                (const uint8_t *)workspace->payload, &result->memory_sequence);
            if (status != SPG_OK) {
                return status;
            }
        }
    }

    return SPG_OK;
}
