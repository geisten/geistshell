#include "geistshell/sim_executor.h"

#include <stdio.h>
#include <string.h>

static enum spg_status load_sim(const char *text, struct spg_sim_config *sim) {
    struct spg_sexpr_token      tokens[256];
    struct spg_sexpr_node       nodes[256];
    struct spg_sim_config_error error = {};
    return spg_sim_config_load(strlen(text), text, 256u, tokens, 256u, nodes,
                               sim, &error);
}

static struct spg_recommendation simulator_rec(void) {
    struct spg_recommendation rec = {
        .state         = SPG_RECOMMENDATION_VALID,
        .reject_reason = SPG_RECOMMENDATION_REJECT_NONE,
        .action_kind   = SPG_ACTION_SIMULATOR,
        .confidence_bp = 8000u,
    };
    rec.action = (struct spg_action_request){
        .kind         = SPG_ACTION_SIMULATOR,
        .uses_network = false,
        .cost         = 1u,
    };
    return rec;
}

static int test_patch_highest_vulnerability(void) {
    const char *text =
        "(scenario"
        " (host (id web) (criticality_bp 8000))"
        " (service (id ssh_web) (host web) (name ssh) (port 22) (exposure_bp 5000))"
        " (service (id http_web) (host web) (name http) (port 80) (exposure_bp 9000))"
        " (vulnerability (id low) (service ssh_web) (severity_bp 2000) (patched false))"
        " (vulnerability (id high) (service http_web) (severity_bp 9000) (patched false)))";
    struct spg_sim_config sim = {};
    if (load_sim(text, &sim) != SPG_OK) {
        return 1;
    }
    char payload[512];
    const struct spg_sim_executor_workspace workspace = {
        .payload_capacity = sizeof payload,
        .payload          = payload,
    };
    struct spg_sim_executor_state state = {
        .sim = &sim,
    };
    const struct spg_sim_executor_config config = {};
    const struct spg_policy_decision decision = {
        .kind = SPG_POLICY_DECISION_ALLOW,
    };
    struct spg_recommendation rec = simulator_rec();
    struct spg_sim_executor_result result = {};
    if (spg_sim_executor_step(&state, &config, &rec, &decision, &workspace,
                              &result) != SPG_OK) {
        return 1;
    }
    if (result.action != SPG_SIM_EXEC_PATCH_VULNERABILITY ||
        result.selected_index != 1u || !result.mutated) {
        return 1;
    }
    if (!sim.vulnerabilities[1u].patched ||
        result.risk_after.total >= result.risk_before.total) {
        return 1;
    }
    return strstr(payload, "(action patch_vulnerability)") != nullptr ? 0 : 1;
}

static int test_disable_account_when_no_vulns(void) {
    const char *text =
        "(scenario"
        " (host (id web) (criticality_bp 8000))"
        " (account (id user) (host web) (username user) (enabled true))"
        " (account (id root) (host web) (username root) (enabled true))"
        " (credential (id c_user) (account user) (strength_bp 9000))"
        " (credential (id c_root) (account root) (strength_bp 1000)))";
    struct spg_sim_config sim = {};
    if (load_sim(text, &sim) != SPG_OK) {
        return 1;
    }
    char payload[512];
    const struct spg_sim_executor_workspace workspace = {
        .payload_capacity = sizeof payload,
        .payload          = payload,
    };
    struct spg_sim_executor_state state = {
        .sim = &sim,
    };
    const struct spg_sim_executor_config config = {};
    const struct spg_policy_decision decision = {
        .kind = SPG_POLICY_DECISION_ALLOW,
    };
    struct spg_recommendation rec = simulator_rec();
    struct spg_sim_executor_result result = {};
    if (spg_sim_executor_step(&state, &config, &rec, &decision, &workspace,
                              &result) != SPG_OK) {
        return 1;
    }
    return result.action == SPG_SIM_EXEC_DISABLE_ACCOUNT &&
                   result.selected_index == 1u && !sim.accounts[1u].enabled
               ? 0
               : 1;
}

static int test_noop_when_no_mutation_available(void) {
    const char *text = "(scenario (host (id solo) (criticality_bp 1000)))";
    struct spg_sim_config sim = {};
    if (load_sim(text, &sim) != SPG_OK) {
        return 1;
    }
    char payload[512];
    const struct spg_sim_executor_workspace workspace = {
        .payload_capacity = sizeof payload,
        .payload          = payload,
    };
    struct spg_sim_executor_state state = {
        .sim = &sim,
    };
    const struct spg_sim_executor_config config = {};
    const struct spg_policy_decision decision = {
        .kind = SPG_POLICY_DECISION_ALLOW,
    };
    struct spg_recommendation rec = simulator_rec();
    struct spg_sim_executor_result result = {};
    if (spg_sim_executor_step(&state, &config, &rec, &decision, &workspace,
                              &result) != SPG_OK) {
        return 1;
    }
    return result.action == SPG_SIM_EXEC_NOOP && !result.mutated &&
                   result.risk_after.total == result.risk_before.total
               ? 0
               : 1;
}

static int test_journal_graph_memory(void) {
    const char journal_path[] = "/tmp/geistshell_test_sim_executor.journal";
    (void)remove(journal_path);
    const char *text =
        "(scenario"
        " (host (id web) (criticality_bp 8000))"
        " (service (id ssh_web) (host web) (name ssh) (port 22) (exposure_bp 5000))"
        " (vulnerability (id cve) (service ssh_web) (severity_bp 8000) (patched false)))";
    struct spg_sim_config sim = {};
    struct spg_graph graph = {};
    struct spg_memory memory = {};
    struct spg_journal_writer writer = {};
    if (load_sim(text, &sim) != SPG_OK ||
        spg_journal_writer_open(&writer, journal_path) != SPG_OK) {
        return 1;
    }
    spg_graph_init(&graph);
    spg_memory_init(&memory);
    struct spg_node_id recommendation_node = {};
    if (spg_graph_add_node(&graph, SPG_GRAPH_NODE_PLAN, 4u,
                           (struct spg_text_span){.offset = 0u, .length = 4u},
                           &recommendation_node) != SPG_OK) {
        (void)spg_journal_writer_close(&writer);
        return 1;
    }
    char payload[512];
    const struct spg_sim_executor_workspace workspace = {
        .payload_capacity = sizeof payload,
        .payload          = payload,
    };
    struct spg_sim_executor_state state = {
        .sim     = &sim,
        .journal = &writer,
        .graph   = &graph,
        .memory  = &memory,
    };
    const struct spg_sim_executor_config config = {
        .actor_id                = 4u,
        .timestamp_ns            = 9u,
        .write_journal           = true,
        .update_graph            = true,
        .update_memory           = true,
        .has_recommendation_node = true,
        .recommendation_node     = recommendation_node,
    };
    const struct spg_policy_decision decision = {
        .kind = SPG_POLICY_DECISION_ALLOW,
    };
    struct spg_recommendation rec = simulator_rec();
    struct spg_sim_executor_result result = {};
    if (spg_sim_executor_step(&state, &config, &rec, &decision, &workspace,
                              &result) != SPG_OK) {
        (void)spg_journal_writer_close(&writer);
        return 1;
    }
    if (result.sim_sequence != 1u || result.graph_sequence != 2u ||
        result.memory_sequence != 3u) {
        (void)spg_journal_writer_close(&writer);
        return 1;
    }
    if (!result.has_result_node || !result.has_result_edge ||
        !result.has_memory_fact || graph.node_count != 2u ||
        graph.edge_count != 1u || memory.fact_count != 1u) {
        (void)spg_journal_writer_close(&writer);
        return 1;
    }
    if (spg_journal_writer_close(&writer) != SPG_OK) {
        return 1;
    }

    struct spg_journal_reader reader = {};
    if (spg_journal_reader_open(&reader, journal_path) != SPG_OK) {
        return 1;
    }
    uint8_t payload_read[512];
    struct spg_journal_record record = {};
    const enum spg_journal_event_kind expected[3] = {
        SPG_JOURNAL_EVENT_SIM,
        SPG_JOURNAL_EVENT_GRAPH,
        SPG_JOURNAL_EVENT_MEMORY,
    };
    for (size_t i = 0u; i < 3u; i += 1u) {
        if (spg_journal_reader_next(&reader, sizeof payload_read, payload_read,
                                    &record) != SPG_OK) {
            (void)spg_journal_reader_close(&reader);
            return 1;
        }
        if (record.header.event_kind != (uint32_t)expected[i]) {
            (void)spg_journal_reader_close(&reader);
            return 1;
        }
    }
    if (spg_journal_reader_close(&reader) != SPG_OK) {
        return 1;
    }
    (void)remove(journal_path);
    return 0;
}

static int test_policy_denied_invalid(void) {
    struct spg_sim_config sim = {};
    char payload[128];
    const struct spg_sim_executor_workspace workspace = {
        .payload_capacity = sizeof payload,
        .payload          = payload,
    };
    struct spg_sim_executor_state state = {
        .sim = &sim,
    };
    const struct spg_sim_executor_config config = {};
    const struct spg_policy_decision decision = {
        .kind = SPG_POLICY_DECISION_DENY,
    };
    struct spg_recommendation rec = simulator_rec();
    struct spg_sim_executor_result result = {};
    return spg_sim_executor_step(&state, &config, &rec, &decision, &workspace,
                                 &result) == SPG_E_INVALID_ARG
               ? 0
               : 1;
}

static int test_payload_limit_after_mutation(void) {
    const char *text =
        "(scenario"
        " (host (id web) (criticality_bp 8000))"
        " (service (id ssh_web) (host web) (name ssh) (port 22) (exposure_bp 5000))"
        " (vulnerability (id cve) (service ssh_web) (severity_bp 8000) (patched false)))";
    struct spg_sim_config sim = {};
    if (load_sim(text, &sim) != SPG_OK) {
        return 1;
    }
    char payload[8];
    const struct spg_sim_executor_workspace workspace = {
        .payload_capacity = sizeof payload,
        .payload          = payload,
    };
    struct spg_sim_executor_state state = {
        .sim = &sim,
    };
    const struct spg_sim_executor_config config = {};
    const struct spg_policy_decision decision = {
        .kind = SPG_POLICY_DECISION_ALLOW,
    };
    struct spg_recommendation rec = simulator_rec();
    struct spg_sim_executor_result result = {};
    const enum spg_status status =
        spg_sim_executor_step(&state, &config, &rec, &decision, &workspace,
                              &result);
    return status == SPG_E_LIMIT && result.payload_truncated &&
                   !sim.vulnerabilities[0u].patched
               ? 0
               : 1;
}

static int test_invalid_args(void) {
    struct spg_sim_config sim = {};
    char payload[128];
    const struct spg_sim_executor_workspace workspace = {
        .payload_capacity = sizeof payload,
        .payload          = payload,
    };
    struct spg_sim_executor_state state = {
        .sim = &sim,
    };
    const struct spg_sim_executor_config config = {};
    const struct spg_policy_decision decision = {
        .kind = SPG_POLICY_DECISION_ALLOW,
    };
    struct spg_recommendation rec = simulator_rec();
    struct spg_sim_executor_result result = {};
    if (spg_sim_executor_step(nullptr, &config, &rec, &decision, &workspace,
                              &result) != SPG_E_INVALID_ARG) {
        return 1;
    }
    if (spg_sim_executor_step(&state, &config, &rec, &decision, nullptr,
                              &result) != SPG_E_INVALID_ARG) {
        return 1;
    }
    if (spg_sim_exec_action_to_string(SPG_SIM_EXEC_PATCH_VULNERABILITY) ==
        nullptr) {
        return 1;
    }
    return 0;
}

int main(void) {
    if (test_patch_highest_vulnerability() != 0) {
        fprintf(stderr, "test_patch_highest_vulnerability failed\n");
        return 1;
    }
    if (test_disable_account_when_no_vulns() != 0) {
        fprintf(stderr, "test_disable_account_when_no_vulns failed\n");
        return 1;
    }
    if (test_noop_when_no_mutation_available() != 0) {
        fprintf(stderr, "test_noop_when_no_mutation_available failed\n");
        return 1;
    }
    if (test_journal_graph_memory() != 0) {
        fprintf(stderr, "test_journal_graph_memory failed\n");
        return 1;
    }
    if (test_policy_denied_invalid() != 0) {
        fprintf(stderr, "test_policy_denied_invalid failed\n");
        return 1;
    }
    if (test_payload_limit_after_mutation() != 0) {
        fprintf(stderr, "test_payload_limit_after_mutation failed\n");
        return 1;
    }
    if (test_invalid_args() != 0) {
        fprintf(stderr, "test_invalid_args failed\n");
        return 1;
    }
    return 0;
}
