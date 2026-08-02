#include "geistshell/actor.h"

#include <stdio.h>
#include <string.h>

static int test_actor_step_updates_state_and_journal(void) {
    const char journal_path[] = "/tmp/geistshell_test_actor_step.journal";
    (void)remove(journal_path);

    struct spg_graph          graph = {};
    struct spg_memory         memory = {};
    struct spg_journal_writer writer = {};
    struct spg_model_adapter  model = {};
    spg_graph_init(&graph);
    spg_memory_init(&memory);

    const char response[] = "(recommend (kind simulator) (reason safe))";
    const struct spg_model_adapter_config model_config = {
        .kind            = SPG_MODEL_ADAPTER_FAKE,
        .sampling        = {.top_p = 1.0f},
        .fake_response_n = sizeof response - 1u,
        .fake_response   = response,
    };
    if (spg_model_adapter_init(&model, &model_config) != SPG_OK) {
        return 1;
    }
    if (spg_journal_writer_open(&writer, journal_path) != SPG_OK) {
        spg_model_adapter_destroy(&model);
        return 1;
    }

    struct spg_context_graph_ref   graph_refs[4];
    struct spg_context_memory_ref  memory_refs[4];
    struct spg_context_journal_ref journal_refs[4];
    char context[2048];
    char output[256];
    const struct spg_actor_step_workspace workspace = {
        .context_capacity      = sizeof context,
        .context               = context,
        .model_output_capacity = sizeof output,
        .model_output          = output,
        .graph_ref_capacity    = 4u,
        .graph_refs            = graph_refs,
        .memory_ref_capacity   = 4u,
        .memory_refs           = memory_refs,
        .journal_ref_capacity  = 4u,
        .journal_refs          = journal_refs,
    };
    struct spg_actor_state state = {
        .graph   = &graph,
        .memory  = &memory,
        .journal = &writer,
        .model   = &model,
    };
    const struct spg_actor_step_config config = {
        .actor_id            = 7u,
        .timestamp_ns        = 123u,
        .context_limits      = {.graph_nodes = 4u,
                                .memory_facts = 4u,
                                .journal_events = 4u},
        .max_decode_tokens   = 8u,
        .reset_model_session = true,
        .write_journal       = true,
        .update_graph        = true,
        .update_memory       = true,
    };
    struct spg_actor_step_result result = {};
    if (spg_actor_step(&state, &config, &workspace, &result) != SPG_OK) {
        (void)spg_journal_writer_close(&writer);
        spg_model_adapter_destroy(&model);
        return 1;
    }
    if (strcmp(output, response) != 0 || result.model_output_n !=
                                             sizeof response - 1u) {
        (void)spg_journal_writer_close(&writer);
        spg_model_adapter_destroy(&model);
        return 1;
    }
    if (graph.node_count != 2u || graph.edge_count != 1u ||
        memory.fact_count != 1u) {
        (void)spg_journal_writer_close(&writer);
        spg_model_adapter_destroy(&model);
        return 1;
    }
    if (!result.has_model_input_node || !result.has_recommendation_node ||
        !result.has_recommendation_fact) {
        (void)spg_journal_writer_close(&writer);
        spg_model_adapter_destroy(&model);
        return 1;
    }
    if (result.model_input_sequence != 1u ||
        result.model_output_sequence != 2u || result.graph_sequence != 3u ||
        result.memory_sequence != 4u) {
        (void)spg_journal_writer_close(&writer);
        spg_model_adapter_destroy(&model);
        return 1;
    }
    if (memory.facts[result.recommendation_fact.index].source_event_id != 2u) {
        (void)spg_journal_writer_close(&writer);
        spg_model_adapter_destroy(&model);
        return 1;
    }

    if (spg_journal_writer_close(&writer) != SPG_OK) {
        spg_model_adapter_destroy(&model);
        return 1;
    }
    spg_model_adapter_destroy(&model);

    struct spg_journal_reader reader = {};
    if (spg_journal_reader_open(&reader, journal_path) != SPG_OK) {
        return 1;
    }
    uint8_t payload[2048];
    struct spg_journal_record record = {};
    const enum spg_journal_event_kind expected[4] = {
        SPG_JOURNAL_EVENT_MODEL_INPUT,
        SPG_JOURNAL_EVENT_MODEL_OUTPUT,
        SPG_JOURNAL_EVENT_GRAPH,
        SPG_JOURNAL_EVENT_MEMORY,
    };
    for (size_t i = 0u; i < 4u; i += 1u) {
        if (spg_journal_reader_next(&reader, sizeof payload, payload,
                                    &record) != SPG_OK) {
            (void)spg_journal_reader_close(&reader);
            return 1;
        }
        if (record.header.sequence != i + 1u ||
            record.header.event_kind != (uint32_t)expected[i]) {
            (void)spg_journal_reader_close(&reader);
            return 1;
        }
    }
    if (spg_journal_reader_next(&reader, sizeof payload, payload, &record) !=
        SPG_E_NOT_FOUND) {
        (void)spg_journal_reader_close(&reader);
        return 1;
    }
    if (spg_journal_reader_close(&reader) != SPG_OK) {
        return 1;
    }
    (void)remove(journal_path);
    return 0;
}

static int test_context_buffer_limit_stops_before_model(void) {
    struct spg_graph         graph = {};
    struct spg_memory        memory = {};
    struct spg_model_adapter model = {};
    spg_graph_init(&graph);
    spg_memory_init(&memory);

    const char response[] = "x";
    const struct spg_model_adapter_config model_config = {
        .kind            = SPG_MODEL_ADAPTER_FAKE,
        .sampling        = {.top_p = 1.0f},
        .fake_response_n = sizeof response - 1u,
        .fake_response   = response,
    };
    if (spg_model_adapter_init(&model, &model_config) != SPG_OK) {
        return 1;
    }

    struct spg_context_graph_ref   graph_refs[1];
    struct spg_context_memory_ref  memory_refs[1];
    struct spg_context_journal_ref journal_refs[1];
    char context[8];
    char output[8] = "old";
    const struct spg_actor_step_workspace workspace = {
        .context_capacity      = sizeof context,
        .context               = context,
        .model_output_capacity = sizeof output,
        .model_output          = output,
        .graph_ref_capacity    = 1u,
        .graph_refs            = graph_refs,
        .memory_ref_capacity   = 1u,
        .memory_refs           = memory_refs,
        .journal_ref_capacity  = 1u,
        .journal_refs          = journal_refs,
    };
    struct spg_actor_state state = {
        .graph  = &graph,
        .memory = &memory,
        .model  = &model,
    };
    const struct spg_actor_step_config config = {
        .context_limits    = {.graph_nodes = 1u,
                              .memory_facts = 1u,
                              .journal_events = 1u},
        .max_decode_tokens = 1u,
        .update_graph      = true,
        .update_memory     = true,
    };
    struct spg_actor_step_result result = {};
    const enum spg_status status =
        spg_actor_step(&state, &config, &workspace, &result);
    spg_model_adapter_destroy(&model);
    if (status != SPG_E_LIMIT) {
        return 1;
    }
    if (graph.node_count != 0u || memory.fact_count != 0u) {
        return 1;
    }
    return 0;
}

static int test_model_output_limit_is_journaled_as_failure(void) {
    const char journal_path[] = "/tmp/geistshell_test_actor_limit.journal";
    (void)remove(journal_path);

    struct spg_journal_writer writer = {};
    struct spg_model_adapter  model = {};
    const char response[] = "abcdef";
    const struct spg_model_adapter_config model_config = {
        .kind            = SPG_MODEL_ADAPTER_FAKE,
        .sampling        = {.top_p = 1.0f},
        .fake_response_n = sizeof response - 1u,
        .fake_response   = response,
    };
    if (spg_model_adapter_init(&model, &model_config) != SPG_OK) {
        return 1;
    }
    if (spg_journal_writer_open(&writer, journal_path) != SPG_OK) {
        spg_model_adapter_destroy(&model);
        return 1;
    }

    struct spg_context_graph_ref   graph_refs[1];
    struct spg_context_memory_ref  memory_refs[1];
    struct spg_context_journal_ref journal_refs[1];
    char context[32768];
    char output[4];
    const struct spg_actor_step_workspace workspace = {
        .context_capacity      = sizeof context,
        .context               = context,
        .model_output_capacity = sizeof output,
        .model_output          = output,
        .graph_ref_capacity    = 1u,
        .graph_refs            = graph_refs,
        .memory_ref_capacity   = 1u,
        .memory_refs           = memory_refs,
        .journal_ref_capacity  = 1u,
        .journal_refs          = journal_refs,
    };
    struct spg_actor_state state = {
        .journal = &writer,
        .model   = &model,
    };
    const struct spg_actor_step_config config = {
        .context_limits    = {.journal_events = 1u},
        .max_decode_tokens = 1u,
        .write_journal     = true,
    };
    struct spg_actor_step_result result = {};
    if (spg_actor_step(&state, &config, &workspace, &result) != SPG_E_LIMIT) {
        (void)spg_journal_writer_close(&writer);
        spg_model_adapter_destroy(&model);
        return 1;
    }
    if (!result.model_output_truncated || result.model_output_sequence != 2u) {
        (void)spg_journal_writer_close(&writer);
        spg_model_adapter_destroy(&model);
        return 1;
    }
    if (spg_journal_writer_close(&writer) != SPG_OK) {
        spg_model_adapter_destroy(&model);
        return 1;
    }
    spg_model_adapter_destroy(&model);

    struct spg_journal_reader reader = {};
    if (spg_journal_reader_open(&reader, journal_path) != SPG_OK) {
        return 1;
    }
    uint8_t payload[32768];
    struct spg_journal_record record = {};
    if (spg_journal_reader_next(&reader, sizeof payload, payload, &record) !=
        SPG_OK) {
        (void)spg_journal_reader_close(&reader);
        return 1;
    }
    if (spg_journal_reader_next(&reader, sizeof payload, payload, &record) !=
        SPG_OK) {
        (void)spg_journal_reader_close(&reader);
        return 1;
    }
    if (record.header.event_kind != SPG_JOURNAL_EVENT_MODEL_OUTPUT ||
        record.header.status != SPG_E_LIMIT) {
        (void)spg_journal_reader_close(&reader);
        return 1;
    }
    if (spg_journal_reader_close(&reader) != SPG_OK) {
        return 1;
    }
    (void)remove(journal_path);
    return 0;
}

static int test_invalid_args(void) {
    char context[64];
    char output[64];
    struct spg_model_adapter model = {};
    const struct spg_model_adapter_config model_config = {
        .kind     = SPG_MODEL_ADAPTER_FAKE,
        .sampling = {.top_p = 1.0f},
    };
    if (spg_model_adapter_init(&model, &model_config) != SPG_OK) {
        return 1;
    }
    struct spg_actor_state state = {
        .model = &model,
    };
    const struct spg_actor_step_config config = {};
    const struct spg_actor_step_workspace workspace = {
        .context_capacity      = sizeof context,
        .context               = context,
        .model_output_capacity = sizeof output,
        .model_output          = output,
    };
    struct spg_actor_step_result result = {};
    if (spg_actor_step(nullptr, &config, &workspace, &result) !=
        SPG_E_INVALID_ARG) {
        spg_model_adapter_destroy(&model);
        return 1;
    }
    if (spg_actor_step(&state, &config, nullptr, &result) !=
        SPG_E_INVALID_ARG) {
        spg_model_adapter_destroy(&model);
        return 1;
    }
    if (spg_actor_step(&state, &config, &workspace, nullptr) !=
        SPG_E_INVALID_ARG) {
        spg_model_adapter_destroy(&model);
        return 1;
    }
    spg_model_adapter_destroy(&model);
    return 0;
}

int main(void) {
    if (test_actor_step_updates_state_and_journal() != 0) {
        fprintf(stderr, "test_actor_step_updates_state_and_journal failed\n");
        return 1;
    }
    if (test_context_buffer_limit_stops_before_model() != 0) {
        fprintf(stderr, "test_context_buffer_limit_stops_before_model failed\n");
        return 1;
    }
    if (test_model_output_limit_is_journaled_as_failure() != 0) {
        fprintf(stderr,
                "test_model_output_limit_is_journaled_as_failure failed\n");
        return 1;
    }
    if (test_invalid_args() != 0) {
        fprintf(stderr, "test_invalid_args failed\n");
        return 1;
    }
    return 0;
}
