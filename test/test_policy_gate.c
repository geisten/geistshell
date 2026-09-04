#include "geistshell/policy_gate.h"

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
    "  ((name build.run) (kind local_shell) (enabled true) (budget 1))"
    "  ((name sim.act) (kind simulator) (enabled true) (budget 8))))";

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

static struct spg_text_span span_for(const char *needle) {
    const char *found = strstr(policy_text, needle);
    if (found == nullptr) {
        return (struct spg_text_span){};
    }
    return (struct spg_text_span){
        .offset = (size_t)(found - policy_text),
        .length = strlen(needle),
    };
}

static struct spg_recommendation sim_recommendation(void) {
    struct spg_recommendation rec = {
        .state         = SPG_RECOMMENDATION_VALID,
        .reject_reason = SPG_RECOMMENDATION_REJECT_NONE,
        .action_kind   = SPG_ACTION_SIMULATOR,
        .capability    = span_for("sim.act"),
        .confidence_bp = 7000u,
    };
    rec.action = (struct spg_action_request){
        .kind         = SPG_ACTION_SIMULATOR,
        .capability   = rec.capability,
        .uses_network = false,
        .cost         = 1u,
    };
    return rec;
}

static int test_allow_journaled(void) {
    const char journal_path[] = "/tmp/geistshell_test_policy_gate_allow.journal";
    (void)remove(journal_path);

    struct spg_policy_config policy = {};
    struct spg_policy_usage  usage = {};
    struct spg_journal_writer writer = {};
    if (load_policy(&policy) != 0 ||
        spg_journal_writer_open(&writer, journal_path) != SPG_OK) {
        return 1;
    }
    char payload[512];
    const struct spg_policy_gate_workspace workspace = {
        .payload_capacity = sizeof payload,
        .payload          = payload,
    };
    const struct spg_policy_gate_state state = {
        .policy_text_n          = strlen(policy_text),
        .policy_text            = policy_text,
        .recommendation_text_n  = strlen(policy_text),
        .recommendation_text    = policy_text,
        .policy                 = &policy,
        .usage                  = &usage,
        .journal                = &writer,
    };
    const struct spg_policy_gate_config config = {
        .timestamp_ns  = 7u,
        .write_journal = true,
    };
    struct spg_policy_gate_result result = {};
    struct spg_recommendation     rec = sim_recommendation();
    if (spg_policy_gate_step(&state, &config, &rec, &workspace, &result) !=
        SPG_OK) {
        (void)spg_journal_writer_close(&writer);
        return 1;
    }
    if (result.decision.kind != SPG_POLICY_DECISION_ALLOW ||
        result.policy_sequence != 1u || result.has_policy_node) {
        (void)spg_journal_writer_close(&writer);
        return 1;
    }
    if (strstr(payload, "(decision allow)") == nullptr ||
        strstr(payload, "(action_kind simulator)") == nullptr) {
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
    if (spg_journal_reader_next(&reader, sizeof payload_read, payload_read,
                                &record) != SPG_OK) {
        (void)spg_journal_reader_close(&reader);
        return 1;
    }
    if (record.header.event_kind != SPG_JOURNAL_EVENT_POLICY_DECISION ||
        record.header.status != SPG_OK) {
        (void)spg_journal_reader_close(&reader);
        return 1;
    }
    if (spg_journal_reader_close(&reader) != SPG_OK) {
        return 1;
    }
    (void)remove(journal_path);
    return 0;
}

static int test_deny_updates_graph(void) {
    struct spg_policy_config policy = {};
    struct spg_policy_usage  usage = {};
    struct spg_graph         graph = {};
    if (load_policy(&policy) != 0) {
        return 1;
    }
    spg_graph_init(&graph);
    struct spg_node_id recommendation_node = {};
    if (spg_graph_add_node(&graph, SPG_GRAPH_NODE_PLAN, 3u,
                           (struct spg_text_span){.offset = 0u, .length = 5u},
                           &recommendation_node) != SPG_OK) {
        return 1;
    }

    char payload[512];
    const struct spg_policy_gate_workspace workspace = {
        .payload_capacity = sizeof payload,
        .payload          = payload,
    };
    const struct spg_policy_gate_state state = {
        .policy_text_n          = strlen(policy_text),
        .policy_text            = policy_text,
        .recommendation_text_n  = strlen(policy_text),
        .recommendation_text    = policy_text,
        .policy                 = &policy,
        .usage                  = &usage,
        .graph                  = &graph,
    };
    const struct spg_policy_gate_config config = {
        .actor_id                = 3u,
        .update_graph_on_deny    = true,
        .has_recommendation_node = true,
        .recommendation_node     = recommendation_node,
    };
    struct spg_recommendation rec = sim_recommendation();
    rec.action.capability = span_for("build.run");
    rec.capability        = rec.action.capability;

    struct spg_policy_gate_result result = {};
    if (spg_policy_gate_step(&state, &config, &rec, &workspace, &result) !=
        SPG_OK) {
        return 1;
    }
    if (result.decision.kind != SPG_POLICY_DECISION_DENY ||
        result.decision.deny_reason != SPG_POLICY_DENY_KIND_MISMATCH) {
        return 1;
    }
    if (!result.has_policy_node || !result.has_blocked_edge ||
        graph.node_count != 2u || graph.edge_count != 1u) {
        return 1;
    }
    if (graph.nodes[result.policy_node.index].kind !=
            SPG_GRAPH_NODE_POLICY_DECISION ||
        graph.nodes[result.policy_node.index].flags != SPG_GRAPH_NODE_BLOCKED) {
        return 1;
    }
    return graph.edges[result.blocked_edge.index].kind ==
                   SPG_GRAPH_EDGE_BLOCKED_BY_POLICY
               ? 0
               : 1;
}

static int test_deny_journal_status(void) {
    const char journal_path[] = "/tmp/geistshell_test_policy_gate_deny.journal";
    (void)remove(journal_path);
    struct spg_policy_config policy = {};
    struct spg_policy_usage  usage = {};
    struct spg_journal_writer writer = {};
    if (load_policy(&policy) != 0 ||
        spg_journal_writer_open(&writer, journal_path) != SPG_OK) {
        return 1;
    }
    char payload[512];
    const struct spg_policy_gate_workspace workspace = {
        .payload_capacity = sizeof payload,
        .payload          = payload,
    };
    const struct spg_policy_gate_state state = {
        .policy_text_n          = strlen(policy_text),
        .policy_text            = policy_text,
        .recommendation_text_n  = strlen(policy_text),
        .recommendation_text    = policy_text,
        .policy                 = &policy,
        .usage                  = &usage,
        .journal                = &writer,
    };
    const struct spg_policy_gate_config config = {
        .write_journal = true,
    };
    struct spg_recommendation rec = sim_recommendation();
    rec.action.cost = 9u;
    struct spg_policy_gate_result result = {};
    if (spg_policy_gate_step(&state, &config, &rec, &workspace, &result) !=
        SPG_OK) {
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
    if (spg_journal_reader_next(&reader, sizeof payload_read, payload_read,
                                &record) != SPG_OK) {
        (void)spg_journal_reader_close(&reader);
        return 1;
    }
    if (record.header.status != SPG_E_POLICY_DENIED) {
        (void)spg_journal_reader_close(&reader);
        return 1;
    }
    if (spg_journal_reader_close(&reader) != SPG_OK) {
        return 1;
    }
    (void)remove(journal_path);
    return 0;
}

static int test_payload_limit_stops_before_journal(void) {
    struct spg_policy_config policy = {};
    struct spg_policy_usage  usage = {};
    if (load_policy(&policy) != 0) {
        return 1;
    }
    char payload[8];
    const struct spg_policy_gate_workspace workspace = {
        .payload_capacity = sizeof payload,
        .payload          = payload,
    };
    const struct spg_policy_gate_state state = {
        .policy_text_n          = strlen(policy_text),
        .policy_text            = policy_text,
        .recommendation_text_n  = strlen(policy_text),
        .recommendation_text    = policy_text,
        .policy                 = &policy,
        .usage                  = &usage,
    };
    const struct spg_policy_gate_config config = {};
    struct spg_recommendation rec = sim_recommendation();
    struct spg_policy_gate_result result = {};
    const enum spg_status status =
        spg_policy_gate_step(&state, &config, &rec, &workspace, &result);
    return status == SPG_E_LIMIT && result.payload_truncated ? 0 : 1;
}

static int test_rejected_recommendation_invalid(void) {
    struct spg_policy_config policy = {};
    struct spg_policy_usage  usage = {};
    if (load_policy(&policy) != 0) {
        return 1;
    }
    char payload[512];
    const struct spg_policy_gate_workspace workspace = {
        .payload_capacity = sizeof payload,
        .payload          = payload,
    };
    const struct spg_policy_gate_state state = {
        .policy_text_n          = strlen(policy_text),
        .policy_text            = policy_text,
        .recommendation_text_n  = strlen(policy_text),
        .recommendation_text    = policy_text,
        .policy                 = &policy,
        .usage                  = &usage,
    };
    const struct spg_policy_gate_config config = {};
    const struct spg_recommendation rec = {
        .state = SPG_RECOMMENDATION_REJECTED,
    };
    struct spg_policy_gate_result result = {};
    return spg_policy_gate_step(&state, &config, &rec, &workspace, &result) ==
                   SPG_E_INVALID_ARG
               ? 0
               : 1;
}

static int test_invalid_args(void) {
    struct spg_policy_config policy = {};
    struct spg_policy_usage  usage = {};
    char payload[512];
    const struct spg_policy_gate_workspace workspace = {
        .payload_capacity = sizeof payload,
        .payload          = payload,
    };
    const struct spg_policy_gate_state state = {
        .policy_text_n          = strlen(policy_text),
        .policy_text            = policy_text,
        .recommendation_text_n  = strlen(policy_text),
        .recommendation_text    = policy_text,
        .policy                 = &policy,
        .usage                  = &usage,
    };
    const struct spg_policy_gate_config config = {};
    struct spg_recommendation rec = sim_recommendation();
    struct spg_policy_gate_result result = {};
    if (spg_policy_gate_step(nullptr, &config, &rec, &workspace, &result) !=
        SPG_E_INVALID_ARG) {
        return 1;
    }
    if (spg_policy_gate_step(&state, &config, &rec, nullptr, &result) !=
        SPG_E_INVALID_ARG) {
        return 1;
    }
    if (spg_policy_gate_step(&state, &config, &rec, &workspace, nullptr) !=
        SPG_E_INVALID_ARG) {
        return 1;
    }
    return 0;
}

/* #119: a device_write's network need comes from the operator's channel
 * table, never from model text. Under (network_default deny) a network
 * channel is refused before any fork; a local channel keeps working; the
 * absence of a table changes nothing (the executor reports NO_DEVICE). */
static const char device_policy_deny[] =
    "(policy"
    " (network_default deny)"
    " (budgets"
    "  (inference_steps 100)"
    "  (tokens 4096)"
    "  (shell_actions 0)"
    "  (sim_actions 0)"
    "  (wall_ms 60000)"
    "  (machine_actions 4))"
    " (capability"
    "  ((name device) (kind device) (enabled true) (budget 4))))";
static const char device_policy_allow[] =
    "(policy"
    " (network_default allow)"
    " (budgets"
    "  (inference_steps 100)"
    "  (tokens 4096)"
    "  (shell_actions 0)"
    "  (sim_actions 0)"
    "  (wall_ms 60000)"
    "  (machine_actions 4))"
    " (capability"
    "  ((name device) (kind device) (enabled true) (budget 4))))";
static const char device_rec_text[] = "device mqtt_heater local_heater";

static struct spg_text_span rec_span(const char *needle) {
    const char *found = strstr(device_rec_text, needle);
    if (found == nullptr) {
        return (struct spg_text_span){};
    }
    return (struct spg_text_span){.offset =
                                      (size_t)(found - device_rec_text),
                                  .length = strlen(needle)};
}

static struct spg_recommendation device_recommendation(const char *channel) {
    struct spg_recommendation rec = {
        .state         = SPG_RECOMMENDATION_VALID,
        .reject_reason = SPG_RECOMMENDATION_REJECT_NONE,
        .action_kind   = SPG_ACTION_DEVICE_WRITE,
        .capability    = rec_span("device"),
        .confidence_bp = 9000u,
        .target        = rec_span(channel),
        .has_target    = true,
        .device_value  = 40,
        .has_device_value = true,
    };
    rec.action = (struct spg_action_request){
        .kind       = SPG_ACTION_DEVICE_WRITE,
        .capability = rec.capability,
        /* what the parser guarantees: the FORM always says false */
        .uses_network = false,
        .cost         = 1u,
    };
    return rec;
}

static int gate_device(const char *policy_src, const struct spg_device *dev,
                       const char                 *channel,
                       struct spg_policy_decision *out) {
    struct spg_sexpr_token         tokens[192];
    struct spg_sexpr_node          nodes[192];
    struct spg_policy_config_error error  = {};
    struct spg_policy_config       policy = {};
    if (spg_policy_config_load(strlen(policy_src), policy_src, 192u, tokens,
                               192u, nodes, &policy, &error) != SPG_OK) {
        return 1;
    }
    struct spg_policy_usage                usage   = {};
    char                                   payload[512];
    const struct spg_policy_gate_workspace workspace = {
        .payload_capacity = sizeof payload,
        .payload          = payload,
    };
    const struct spg_policy_gate_state state = {
        .policy_text_n         = strlen(policy_src),
        .policy_text           = policy_src,
        .recommendation_text_n = strlen(device_rec_text),
        .recommendation_text   = device_rec_text,
        .policy                = &policy,
        .usage                 = &usage,
        .device                = dev,
    };
    const struct spg_policy_gate_config config = {.timestamp_ns = 1u};
    struct spg_policy_gate_result       result = {};
    struct spg_recommendation rec = device_recommendation(channel);
    if (spg_policy_gate_step(&state, &config, &rec, &workspace, &result) !=
        SPG_OK) {
        return 1;
    }
    *out = result.decision;
    return 0;
}

static int test_device_network_from_config(void) {
    struct spg_device dev = {};
    spg_device_init(&dev);
    const struct spg_device_channel mqtt  = {.name     = "mqtt_heater",
                                             .program  = "/nonexistent/mqtt",
                                             .min      = 0,
                                             .max      = 100,
                                             .writable = true,
                                             .safe     = 0,
                                             .network  = true};
    const struct spg_device_channel local = {.name     = "local_heater",
                                             .program  = "/nonexistent/local",
                                             .min      = 0,
                                             .max      = 100,
                                             .writable = true,
                                             .safe     = 0};
    if (spg_device_add_channel(&dev, &mqtt) != SPG_OK ||
        spg_device_add_channel(&dev, &local) != SPG_OK) {
        return 1;
    }

    struct spg_policy_decision decision = {};
    /* Network channel + default deny: refused, with the network reason —
     * before any fork, no matter what uses_network the model's form said. */
    if (gate_device(device_policy_deny, &dev, "mqtt_heater", &decision) != 0 ||
        decision.kind != SPG_POLICY_DECISION_DENY ||
        decision.deny_reason != SPG_POLICY_DENY_NETWORK) {
        return 1;
    }
    /* Local channel + default deny: usable. */
    if (gate_device(device_policy_deny, &dev, "local_heater", &decision) !=
            0 ||
        decision.kind != SPG_POLICY_DECISION_ALLOW) {
        return 1;
    }
    /* Network channel + explicit allow: the device capability still gates it,
     * and here it passes both checks. */
    if (gate_device(device_policy_allow, &dev, "mqtt_heater", &decision) !=
            0 ||
        decision.kind != SPG_POLICY_DECISION_ALLOW) {
        return 1;
    }
    /* No table on the run: nothing to derive from, the gate allows and the
     * executor reports NO_DEVICE downstream — nothing forks either way. */
    if (gate_device(device_policy_deny, nullptr, "mqtt_heater", &decision) !=
            0 ||
        decision.kind != SPG_POLICY_DECISION_ALLOW) {
        return 1;
    }
    return 0;
}

int main(void) {
    if (test_allow_journaled() != 0) {
        fprintf(stderr, "test_allow_journaled failed\n");
        return 1;
    }
    if (test_deny_updates_graph() != 0) {
        fprintf(stderr, "test_deny_updates_graph failed\n");
        return 1;
    }
    if (test_deny_journal_status() != 0) {
        fprintf(stderr, "test_deny_journal_status failed\n");
        return 1;
    }
    if (test_payload_limit_stops_before_journal() != 0) {
        fprintf(stderr, "test_payload_limit_stops_before_journal failed\n");
        return 1;
    }
    if (test_rejected_recommendation_invalid() != 0) {
        fprintf(stderr, "test_rejected_recommendation_invalid failed\n");
        return 1;
    }
    if (test_invalid_args() != 0) {
        fprintf(stderr, "test_invalid_args failed\n");
        return 1;
    }
    if (test_device_network_from_config() != 0) {
        fprintf(stderr, "test_device_network_from_config failed\n");
        return 1;
    }
    return 0;
}
