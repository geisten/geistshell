#include "geistshell/orchestrator.h"

#include <stdio.h>
#include <string.h>

static const char policy_text[] =
    "(policy"
    " (network_default deny)"
    " (budgets"
    "  (inference_steps 100)"
    "  (tokens 4096)"
    "  (shell_actions 1)"
    "  (sim_actions 8)"
    "  (wall_ms 60000))"
    " (capability"
    "  ((name sim.act) (kind simulator) (enabled true) (budget 8))"
    "  ((name build.run) (kind local_shell) (enabled true) (budget 1))))";

static int load_policy(struct spg_policy_config *policy) {
    struct spg_sexpr_token         tokens[192];
    struct spg_sexpr_node          nodes[192];
    struct spg_policy_config_error error = {};
    return spg_policy_config_load(strlen(policy_text), policy_text, 192u,
                                  tokens, 192u, nodes, policy, &error) ==
                   SPG_OK
               ? 0
               : 1;
}

static int load_sim(struct spg_sim_config *sim) {
    const char *text =
        "(scenario"
        " (host (id web) (criticality_bp 8000))"
        " (service (id ssh_web) (host web) (name ssh) (port 22) (exposure_bp 5000))"
        " (vulnerability (id cve) (service ssh_web) (severity_bp 8000) (patched false)))";
    struct spg_sexpr_token      tokens[256];
    struct spg_sexpr_node       nodes[256];
    struct spg_sim_config_error error = {};
    return spg_sim_config_load(strlen(text), text, 256u, tokens, 256u, nodes,
                               sim, &error) == SPG_OK
               ? 0
               : 1;
}

struct test_buffers {
    struct spg_context_graph_ref   graph_refs[8];
    struct spg_context_memory_ref  memory_refs[8];
    struct spg_context_journal_ref journal_refs[8];
    char context[4096];
    char model_output[1024];
    struct spg_sexpr_token rec_tokens[128];
    struct spg_sexpr_node  rec_nodes[128];
    char policy_payload[1024];
    char sim_payload[1024];
    char observation[4096];
    char shell_stdout[4096];
    char shell_stderr[1024];
};

static struct spg_orchestrator_workspace workspace_for(
    struct test_buffers *buffers) {
    return (struct spg_orchestrator_workspace){
        .actor = {
            .context_capacity      = sizeof buffers->context,
            .context               = buffers->context,
            .model_output_capacity = sizeof buffers->model_output,
            .model_output          = buffers->model_output,
            .graph_ref_capacity    = 8u,
            .graph_refs            = buffers->graph_refs,
            .memory_ref_capacity   = 8u,
            .memory_refs           = buffers->memory_refs,
            .journal_ref_capacity  = 8u,
            .journal_refs          = buffers->journal_refs,
        },
        .recommendation_token_capacity = 128u,
        .recommendation_tokens         = buffers->rec_tokens,
        .recommendation_node_capacity  = 128u,
        .recommendation_nodes          = buffers->rec_nodes,
        .policy_payload_capacity       = sizeof buffers->policy_payload,
        .policy_payload                = buffers->policy_payload,
        .sim_payload_capacity          = sizeof buffers->sim_payload,
        .sim_payload                   = buffers->sim_payload,
        .observation_capacity        = sizeof buffers->observation,
        .observation_buf             = buffers->observation,
        .shell_stdout_capacity         = sizeof buffers->shell_stdout,
        .shell_stdout_buf              = buffers->shell_stdout,
        .shell_stderr_capacity         = sizeof buffers->shell_stderr,
        .shell_stderr_buf              = buffers->shell_stderr,
    };
}

static int test_simulator_tick_executes(void) {
    const char journal_path[] = "/tmp/geistshell_test_orchestrator_sim.journal";
    (void)remove(journal_path);

    struct spg_policy_config policy = {};
    struct spg_policy_usage  usage = {};
    struct spg_sim_config    sim = {};
    struct spg_graph         graph = {};
    struct spg_memory        memory = {};
    struct spg_journal_writer writer = {};
    struct spg_model_adapter model = {};
    spg_graph_init(&graph);
    spg_memory_init(&memory);

    const char response[] =
        "(recommend (kind simulator) (capability \"sim.act\") (cost 1) "
        "(uses_network false) (confidence_bp 7000) (reason \"reduce risk\"))";
    const struct spg_model_adapter_config model_config = {
        .kind            = SPG_MODEL_ADAPTER_FAKE,
        .sampling        = {.top_p = 1.0f},
        .fake_response_n = sizeof response - 1u,
        .fake_response   = response,
    };
    if (load_policy(&policy) != 0 || load_sim(&sim) != 0 ||
        spg_model_adapter_init(&model, &model_config) != SPG_OK ||
        spg_journal_writer_open(&writer, journal_path) != SPG_OK) {
        spg_model_adapter_destroy(&model);
        return 1;
    }

    struct test_buffers buffers = {};
    const struct spg_orchestrator_workspace workspace = workspace_for(&buffers);
    struct spg_orchestrator_state state = {
        .graph         = &graph,
        .memory        = &memory,
        .journal       = &writer,
        .model         = &model,
        .sim           = &sim,
        .usage         = &usage,
        .policy        = &policy,
        .policy_text_n = strlen(policy_text),
        .policy_text   = policy_text,
    };
    const struct spg_orchestrator_config config = {
        .actor_id            = 11u,
        .timestamp_ns        = 100u,
        .context_limits      = {.graph_nodes = 8u,
                                .memory_facts = 8u,
                                .journal_events = 8u},
        .max_decode_tokens   = 4u,
        .reset_model_session = true,
        .write_journal       = true,
        .update_graph        = true,
        .update_memory       = true,
    };
    struct spg_orchestrator_result result = {};
    if (spg_orchestrator_tick(&state, &config, &workspace, &result) !=
        SPG_OK) {
        (void)spg_journal_writer_close(&writer);
        spg_model_adapter_destroy(&model);
        return 1;
    }
    if (result.stage != SPG_ORCHESTRATOR_STAGE_SIM_EXECUTED ||
        !spg_orchestrator_recommendation_valid(&result) ||
        !spg_orchestrator_policy_evaluated(&result) ||
        !spg_orchestrator_sim_executed(&result)) {
        (void)spg_journal_writer_close(&writer);
        spg_model_adapter_destroy(&model);
        return 1;
    }
    if (result.policy_gate.decision.kind != SPG_POLICY_DECISION_ALLOW ||
        result.sim.action != SPG_SIM_EXEC_PATCH_VULNERABILITY ||
        !sim.vulnerabilities[0u].patched) {
        (void)spg_journal_writer_close(&writer);
        spg_model_adapter_destroy(&model);
        return 1;
    }
    if (result.actor.model_output_sequence != 2u ||
        result.policy_gate.policy_sequence != 5u ||
        result.sim.sim_sequence != 6u || result.sim.graph_sequence != 7u ||
        result.sim.memory_sequence != 8u) {
        (void)spg_journal_writer_close(&writer);
        spg_model_adapter_destroy(&model);
        return 1;
    }
    if (graph.node_count != 3u || graph.edge_count != 2u ||
        memory.fact_count != 2u) {
        (void)spg_journal_writer_close(&writer);
        spg_model_adapter_destroy(&model);
        return 1;
    }
    if (spg_journal_writer_close(&writer) != SPG_OK) {
        spg_model_adapter_destroy(&model);
        return 1;
    }
    spg_model_adapter_destroy(&model);
    (void)remove(journal_path);
    return 0;
}

static int test_local_shell_executes_under_governance(void) {
    struct spg_policy_config policy = {};
    struct spg_policy_usage  usage = {};
    struct spg_sim_config    sim = {};
    struct spg_graph         graph = {};
    struct spg_memory        memory = {};
    struct spg_model_adapter model = {};
    spg_graph_init(&graph);
    spg_memory_init(&memory);
    const char response[] =
        "(recommend (kind local_shell) (capability \"build.run\") (cost 1) "
        "(uses_network false) (confidence_bp 5000) (reason \"inspect\") "
        "(command \"uname -a\"))";
    const struct spg_model_adapter_config model_config = {
        .kind            = SPG_MODEL_ADAPTER_FAKE,
        .sampling        = {.top_p = 1.0f},
        .fake_response_n = sizeof response - 1u,
        .fake_response   = response,
    };
    if (load_policy(&policy) != 0 || load_sim(&sim) != 0 ||
        spg_model_adapter_init(&model, &model_config) != SPG_OK) {
        spg_model_adapter_destroy(&model);
        return 1;
    }
    struct test_buffers buffers = {};
    const struct spg_orchestrator_workspace workspace = workspace_for(&buffers);
    struct spg_orchestrator_state state = {
        .graph         = &graph,
        .memory        = &memory,
        .model         = &model,
        .sim           = &sim,
        .usage         = &usage,
        .policy        = &policy,
        .policy_text_n = strlen(policy_text),
        .policy_text   = policy_text,
    };
    const struct spg_orchestrator_config config = {
        .actor_id          = 1u,
        .context_limits    = {.graph_nodes = 8u,
                              .memory_facts = 8u,
                              .journal_events = 8u},
        .max_decode_tokens = 2u,
        .update_graph      = true,
        .update_memory     = true,
    };
    struct spg_orchestrator_result result = {};
    if (spg_orchestrator_tick(&state, &config, &workspace, &result) !=
        SPG_OK) {
        spg_model_adapter_destroy(&model);
        return 1;
    }
    spg_model_adapter_destroy(&model);
    /* The shell action is ALLOW'd by policy and now reaches the governed shell
     * executor. With execution_enabled unset (false), the boundary denies it,
     * so it is "executed" (recorded) but not actually run. */
    if (result.stage != SPG_ORCHESTRATOR_STAGE_SHELL_EXECUTED ||
        result.policy_gate.decision.kind != SPG_POLICY_DECISION_ALLOW ||
        !spg_orchestrator_shell_executed(&result) || result.shell.approved ||
        result.shell.boundary_reason !=
            SPG_EXECUTOR_BOUNDARY_EXECUTION_DISABLED ||
        spg_orchestrator_sim_executed(&result)) {
        return 1;
    }
    return !sim.vulnerabilities[0u].patched ? 0 : 1;
}

static int test_local_shell_runs_when_enabled(void) {
    struct spg_policy_config policy = {};
    struct spg_policy_usage  usage = {};
    struct spg_sim_config    sim = {};
    struct spg_graph         graph = {};
    struct spg_memory        memory = {};
    struct spg_model_adapter model = {};
    spg_graph_init(&graph);
    spg_memory_init(&memory);
    const char response[] =
        "(recommend (kind local_shell) (capability \"build.run\") (cost 1) "
        "(uses_network false) (confidence_bp 5000) (reason \"probe\") "
        "(command \"echo orch-shell-ok\"))";
    const struct spg_model_adapter_config model_config = {
        .kind            = SPG_MODEL_ADAPTER_FAKE,
        .sampling        = {.top_p = 1.0f},
        .fake_response_n = sizeof response - 1u,
        .fake_response   = response,
    };
    if (load_policy(&policy) != 0 || load_sim(&sim) != 0 ||
        spg_model_adapter_init(&model, &model_config) != SPG_OK) {
        spg_model_adapter_destroy(&model);
        return 1;
    }
    struct test_buffers buffers = {};
    const struct spg_orchestrator_workspace workspace = workspace_for(&buffers);
    struct spg_orchestrator_state state = {
        .graph         = &graph,
        .memory        = &memory,
        .model         = &model,
        .sim           = &sim,
        .usage         = &usage,
        .policy        = &policy,
        .policy_text_n = strlen(policy_text),
        .policy_text   = policy_text,
    };
    const struct spg_orchestrator_config config = {
        .actor_id               = 1u,
        .context_limits         = {.graph_nodes = 8u,
                                   .memory_facts = 8u,
                                   .journal_events = 8u},
        .max_decode_tokens      = 2u,
        .execution_enabled      = true,
        .exec_working_dir       = ".",
        .exec_workdir_prefix    = ".",
        .exec_timeout_ms        = 5000u,
        .exec_stdout_cap        = sizeof buffers.shell_stdout,
        .exec_stderr_cap        = sizeof buffers.shell_stderr,
    };
    struct spg_orchestrator_result result = {};
    const enum spg_status status =
        spg_orchestrator_tick(&state, &config, &workspace, &result);
    spg_model_adapter_destroy(&model);
    if (status != SPG_OK ||
        result.stage != SPG_ORCHESTRATOR_STAGE_SHELL_EXECUTED ||
        !result.shell.approved || !result.shell.started ||
        result.shell.exit_code != 0) {
        return 1;
    }
    /* The exec output landed in the observation channel (observation buf). */
    return strstr(buffers.observation, "orch-shell-ok") != nullptr ? 0 : 1;
}

static int test_rejected_recommendation_stops_before_policy(void) {
    struct spg_policy_config policy = {};
    struct spg_policy_usage  usage = {};
    struct spg_graph         graph = {};
    struct spg_memory        memory = {};
    struct spg_model_adapter model = {};
    spg_graph_init(&graph);
    spg_memory_init(&memory);
    const char response[] = "not a recommendation";
    const struct spg_model_adapter_config model_config = {
        .kind            = SPG_MODEL_ADAPTER_FAKE,
        .sampling        = {.top_p = 1.0f},
        .fake_response_n = sizeof response - 1u,
        .fake_response   = response,
    };
    if (load_policy(&policy) != 0 ||
        spg_model_adapter_init(&model, &model_config) != SPG_OK) {
        spg_model_adapter_destroy(&model);
        return 1;
    }
    struct test_buffers buffers = {};
    const struct spg_orchestrator_workspace workspace = workspace_for(&buffers);
    struct spg_orchestrator_state state = {
        .graph         = &graph,
        .memory        = &memory,
        .model         = &model,
        .usage         = &usage,
        .policy        = &policy,
        .policy_text_n = strlen(policy_text),
        .policy_text   = policy_text,
    };
    const struct spg_orchestrator_config config = {
        .context_limits    = {.graph_nodes = 8u,
                              .memory_facts = 8u,
                              .journal_events = 8u},
        .max_decode_tokens = 2u,
        .update_graph      = true,
        .update_memory     = true,
    };
    struct spg_orchestrator_result result = {};
    if (spg_orchestrator_tick(&state, &config, &workspace, &result) !=
        SPG_OK) {
        spg_model_adapter_destroy(&model);
        return 1;
    }
    spg_model_adapter_destroy(&model);
    return result.stage == SPG_ORCHESTRATOR_STAGE_RECOMMENDATION_REJECTED &&
                   !spg_orchestrator_policy_evaluated(&result) &&
                   !spg_orchestrator_sim_executed(&result)
               ? 0
               : 1;
}

static int test_unknown_capability_denied_from_model_span(void) {
    struct spg_policy_config policy = {};
    struct spg_policy_usage  usage = {};
    struct spg_graph         graph = {};
    struct spg_memory        memory = {};
    struct spg_model_adapter model = {};
    spg_graph_init(&graph);
    spg_memory_init(&memory);
    const char response[] =
        "(recommend (kind simulator) (capability \"missing.cap\") (cost 1) "
        "(uses_network false) (confidence_bp 7000) (reason \"x\"))";
    const struct spg_model_adapter_config model_config = {
        .kind            = SPG_MODEL_ADAPTER_FAKE,
        .sampling        = {.top_p = 1.0f},
        .fake_response_n = sizeof response - 1u,
        .fake_response   = response,
    };
    if (load_policy(&policy) != 0 ||
        spg_model_adapter_init(&model, &model_config) != SPG_OK) {
        spg_model_adapter_destroy(&model);
        return 1;
    }
    struct test_buffers buffers = {};
    const struct spg_orchestrator_workspace workspace = workspace_for(&buffers);
    struct spg_orchestrator_state state = {
        .graph         = &graph,
        .memory        = &memory,
        .model         = &model,
        .usage         = &usage,
        .policy        = &policy,
        .policy_text_n = strlen(policy_text),
        .policy_text   = policy_text,
    };
    const struct spg_orchestrator_config config = {
        .actor_id          = 1u,
        .context_limits    = {.graph_nodes = 8u,
                              .memory_facts = 8u,
                              .journal_events = 8u},
        .max_decode_tokens = 2u,
        .update_graph      = true,
        .update_memory     = true,
    };
    struct spg_orchestrator_result result = {};
    if (spg_orchestrator_tick(&state, &config, &workspace, &result) !=
        SPG_OK) {
        spg_model_adapter_destroy(&model);
        return 1;
    }
    spg_model_adapter_destroy(&model);
    return result.stage == SPG_ORCHESTRATOR_STAGE_POLICY_GATED &&
                   result.policy_gate.decision.kind ==
                       SPG_POLICY_DECISION_DENY &&
                   result.policy_gate.decision.deny_reason ==
                       SPG_POLICY_DENY_UNKNOWN_CAPABILITY &&
                   !spg_orchestrator_sim_executed(&result)
               ? 0
               : 1;
}

static int test_invalid_args(void) {
    struct test_buffers buffers = {};
    const struct spg_orchestrator_workspace workspace = workspace_for(&buffers);
    struct spg_orchestrator_state state = {};
    struct spg_orchestrator_config config = {};
    struct spg_orchestrator_result result = {};
    if (spg_orchestrator_tick(nullptr, &config, &workspace, &result) !=
        SPG_E_INVALID_ARG) {
        return 1;
    }
    if (spg_orchestrator_tick(&state, &config, &workspace, &result) !=
        SPG_E_INVALID_ARG) {
        return 1;
    }
    return spg_orchestrator_stage_to_string(
               SPG_ORCHESTRATOR_STAGE_SIM_EXECUTED) != nullptr
               ? 0
               : 1;
}

int main(void) {
    if (test_simulator_tick_executes() != 0) {
        fprintf(stderr, "test_simulator_tick_executes failed\n");
        return 1;
    }
    if (test_local_shell_executes_under_governance() != 0) {
        fprintf(stderr, "test_local_shell_executes_under_governance failed\n");
        return 1;
    }
    if (test_local_shell_runs_when_enabled() != 0) {
        fprintf(stderr, "test_local_shell_runs_when_enabled failed\n");
        return 1;
    }
    if (test_rejected_recommendation_stops_before_policy() != 0) {
        fprintf(stderr,
                "test_rejected_recommendation_stops_before_policy failed\n");
        return 1;
    }
    if (test_unknown_capability_denied_from_model_span() != 0) {
        fprintf(stderr,
                "test_unknown_capability_denied_from_model_span failed\n");
        return 1;
    }
    if (test_invalid_args() != 0) {
        fprintf(stderr, "test_invalid_args failed\n");
        return 1;
    }
    return 0;
}
