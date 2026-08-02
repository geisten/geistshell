#include "geistshell/geistshell.h"

#include "geistshell/agent_loop.h"
#include "geistshell/agent_run.h"
#include "geistshell/eval.h"
#include "geistshell/exec_command.h"
#include "geistshell/improve.h"
#include "geistshell/mem_command.h"
#include "geistshell/mem_store.h"

#include <geist.h>

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CLI_TOKEN_CAPACITY 1024u
#define CLI_NODE_CAPACITY 1024u
#define CLI_CONTEXT_BYTES 32768u
#define CLI_MODEL_OUTPUT_BYTES 8192u
#define CLI_PAYLOAD_BYTES 8192u
#define CLI_CONTEXT_REFS 64u
#define CLI_JOURNAL_VERIFY_PAYLOAD_BYTES 8192u
#define CLI_REPLAY_PAYLOAD_BYTES CLI_CONTEXT_BYTES
#define CLI_REPLAY_PREVIEW_BYTES 96u

struct file_buffer {
    size_t n;
    char  *data;
};

static void free_file_buffer(struct file_buffer *buffer);
static enum spg_status load_policy_file(const char *path,
                                        struct file_buffer *policy_text,
                                        struct spg_policy_config *policy);
static enum spg_status load_scenario_file(const char *path,
                                          struct file_buffer *scenario_text,
                                          struct spg_sim_config *sim);

static void print_usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s <command> [args]\n"
            "\n"
            "commands:\n"
            "  version          print geistshell and libgeist versions\n"
            "  exec             run a guarded local command and capture output\n"
            "  memory           store/recall Markdown long-term memories\n"
            "  tick             run one fake-model orchestrator tick\n"
            "  run              run fake-model orchestrator ticks\n"
            "  agent            run a governed multi-step agent loop "
            "(scripted)\n"
            "  eval             score a scripted agent suite (JSONL report)\n"
            "  improve          learn lessons from eval failures into memory\n"
            "  replay           print a journal timeline as JSONL\n"
            "  verify-journal   verify a journal (+ --key <f> checks the seal)\n"
            "  seal-journal     write a keyed HMAC seal over a journal\n"
            "  policy-check     validate and summarize a policy file\n"
            "  sim-validate     validate and summarize a scenario file\n",
            argv0);
}

static void print_tick_usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s tick --run <run.spg> --fake '<recommendation>'\n"
            "\n"
            "Runs exactly one orchestrator tick with a fake model output.\n"
            "Run config supplies policy, scenario and journal paths.\n",
            argv0);
}

static void print_run_usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s run --config <run.spg> [--fake '<recommendation>'] "
            "[--ticks <n>] [--write-sim-state <build/final.spg>] "
            "[--write-run-state <build/run-state.json>] "
            "[--remote-url <url>] [--remote-model <name>]\n"
            "\n"
            "Runs orchestrator ticks with one mutable in-process "
            "simulator state.\n"
            "Without --fake, the model path from the run config is loaded via "
            "libgeist.\n"
            "With --remote-url (or GEISTSHELL_API_URL) and a REMOTE=1 build, an "
            "OpenAI-compatible\nendpoint drives the loop; the key is read from "
            "GEISTSHELL_API_KEY.\n"
            "Shell and network recommendations stop after recommendation and "
            "policy gating.\n"
            "If requested, writes the final simulator state as a scenario "
            "S-expression.\n",
            argv0);
}

static void print_verify_journal_usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s verify-journal <journal.sgj> [--key <keyfile>] "
            "[--sig <path>]\n",
            argv0);
}

static void print_replay_usage(const char *argv0) {
    fprintf(stderr, "usage: %s replay <journal.sgj>\n", argv0);
}

static void print_policy_check_usage(const char *argv0) {
    fprintf(stderr, "usage: %s policy-check <policy.spg>\n", argv0);
}

static void print_sim_validate_usage(const char *argv0) {
    fprintf(stderr, "usage: %s sim-validate <scenario.spg>\n", argv0);
}

static const char *policy_capability_kind_name(
    const enum spg_policy_capability_kind kind) {
    switch (kind) {
    case SPG_POLICY_CAP_LOCAL_SHELL:
        return "local_shell";
    case SPG_POLICY_CAP_SSH_AUTH_PROBE:
        return "ssh_auth_probe";
    case SPG_POLICY_CAP_SIMULATOR:
        return "simulator";
    case SPG_POLICY_CAP_MEMORY:
        return "memory";
    }
    return "unknown";
}

static const char *policy_network_default_name(
    const enum spg_policy_network_default value) {
    switch (value) {
    case SPG_POLICY_NETWORK_DENY:
        return "deny";
    case SPG_POLICY_NETWORK_ALLOW:
        return "allow";
    }
    return "unknown";
}

static void print_span_text(const size_t input_n, const char input[],
                            const struct spg_text_span span) {
    if (input == nullptr || span.offset > input_n ||
        span.length > input_n - span.offset) {
        printf("<invalid>");
        return;
    }
    printf("%.*s", (int)span.length, input + span.offset);
}

static const char *journal_event_kind_name(const uint32_t kind) {
    switch ((enum spg_journal_event_kind)kind) {
    case SPG_JOURNAL_EVENT_RUN_START:
        return "run_start";
    case SPG_JOURNAL_EVENT_POLICY_DECISION:
        return "policy_decision";
    case SPG_JOURNAL_EVENT_MODEL_INPUT:
        return "model_input";
    case SPG_JOURNAL_EVENT_MODEL_OUTPUT:
        return "model_output";
    case SPG_JOURNAL_EVENT_ACTION:
        return "action";
    case SPG_JOURNAL_EVENT_RESULT:
        return "result";
    case SPG_JOURNAL_EVENT_GRAPH:
        return "graph";
    case SPG_JOURNAL_EVENT_MEMORY:
        return "memory";
    case SPG_JOURNAL_EVENT_SIM:
        return "sim";
    case SPG_JOURNAL_EVENT_ERROR:
        return "error";
    }
    return "unknown";
}

static bool replay_span_eq_cstr(const size_t input_n, const char input[],
                                const struct spg_text_span span,
                                const char *expected) {
    if (input == nullptr || expected == nullptr || span.offset > input_n ||
        span.length > input_n - span.offset) {
        return false;
    }
    const size_t expected_n = strlen(expected);
    return span.length == expected_n &&
           memcmp(input + span.offset, expected, expected_n) == 0;
}

static bool replay_field_value(const size_t input_n, const char input[],
                               const struct spg_sexpr_node nodes[static 1],
                               const size_t node_count, const char *form_name,
                               const char *field_name,
                               struct spg_text_span *out) {
    if (input == nullptr || nodes == nullptr || node_count == 0u ||
        form_name == nullptr || field_name == nullptr || out == nullptr) {
        return false;
    }
    const uint32_t form = 0u;
    const uint32_t name = spg_sexpr_first_child(nodes, form);
    if (name == SPG_SEXPR_INVALID_INDEX ||
        !replay_span_eq_cstr(input_n, input, nodes[name].span, form_name)) {
        return false;
    }
    uint32_t field = nodes[name].next_sibling;
    while (field != SPG_SEXPR_INVALID_INDEX) {
        const uint32_t field_name_node = spg_sexpr_first_child(nodes, field);
        if (field_name_node != SPG_SEXPR_INVALID_INDEX &&
            replay_span_eq_cstr(input_n, input, nodes[field_name_node].span,
                                field_name)) {
            const uint32_t value = nodes[field_name_node].next_sibling;
            if (value != SPG_SEXPR_INVALID_INDEX &&
                nodes[value].next_sibling == SPG_SEXPR_INVALID_INDEX) {
                *out = nodes[value].span;
                return true;
            }
            return false;
        }
        field = nodes[field].next_sibling;
    }
    return false;
}

static bool replay_payload_field(const size_t payload_n, const uint8_t payload[],
                                 const char *form_name, const char *field_name,
                                 struct spg_text_span *out) {
    if (payload == nullptr || out == nullptr) {
        return false;
    }
    struct spg_sexpr_token tokens[CLI_TOKEN_CAPACITY];
    struct spg_sexpr_node  nodes[CLI_NODE_CAPACITY];
    struct spg_sexpr_error error = {};
    size_t token_count = 0u;
    size_t node_count = 0u;
    const char *text = (const char *)payload;
    const enum spg_status status = spg_sexpr_parse_text(
        payload_n, text, CLI_TOKEN_CAPACITY, tokens, CLI_NODE_CAPACITY, nodes,
        &token_count, &node_count, &error);
    if (status != SPG_OK) {
        return false;
    }
    (void)token_count;
    return replay_field_value(payload_n, text, nodes, node_count, form_name,
                              field_name, out);
}

static bool payload_span_valid(const size_t payload_n,
                               const struct spg_text_span span) {
    return span.offset <= payload_n && span.length <= payload_n - span.offset;
}

static bool payload_span_is_uint(const size_t payload_n,
                                 const uint8_t payload[],
                                 const struct spg_text_span span) {
    if (payload == nullptr || !payload_span_valid(payload_n, span) ||
        span.length == 0u) {
        return false;
    }
    for (size_t i = 0u; i < span.length; i += 1u) {
        const unsigned char ch = payload[span.offset + i];
        if (ch < '0' || ch > '9') {
            return false;
        }
    }
    return true;
}

static bool payload_span_is_bool(const size_t payload_n,
                                 const uint8_t payload[],
                                 const struct spg_text_span span) {
    return replay_span_eq_cstr(payload_n, (const char *)payload, span, "true") ||
           replay_span_eq_cstr(payload_n, (const char *)payload, span, "false");
}

static void print_json_bytes(const size_t bytes_n, const uint8_t bytes[]) {
    printf("\"");
    if (bytes != nullptr) {
        for (size_t i = 0u; i < bytes_n; i += 1u) {
            const unsigned char ch = bytes[i];
            if (ch == '"' || ch == '\\') {
                printf("\\%c", ch);
            } else if (ch == '\n') {
                printf("\\n");
            } else if (ch == '\r') {
                printf("\\r");
            } else if (ch == '\t') {
                printf("\\t");
            } else if (ch >= 32u && ch <= 126u) {
                printf("%c", ch);
            } else {
                printf("\\u%04x", (unsigned int)ch);
            }
        }
    }
    printf("\"");
}

static void print_json_span_string(const size_t payload_n,
                                   const uint8_t payload[],
                                   const struct spg_text_span span) {
    if (payload == nullptr || !payload_span_valid(payload_n, span)) {
        printf("null");
        return;
    }
    print_json_bytes(span.length, payload + span.offset);
}

static void print_json_span_value(const size_t payload_n,
                                  const uint8_t payload[],
                                  const struct spg_text_span span) {
    if (payload == nullptr || !payload_span_valid(payload_n, span)) {
        printf("null");
        return;
    }
    if (payload_span_is_uint(payload_n, payload, span) ||
        payload_span_is_bool(payload_n, payload, span)) {
        printf("%.*s", (int)span.length, (const char *)payload + span.offset);
        return;
    }
    print_json_span_string(payload_n, payload, span);
}

static void print_json_preview(const size_t payload_n, const uint8_t payload[]) {
    const size_t n =
        payload_n < CLI_REPLAY_PREVIEW_BYTES ? payload_n : CLI_REPLAY_PREVIEW_BYTES;
    print_json_bytes(n, payload);
}

static int verify_journal_command(const char *path) {
    if (path == nullptr) {
        return 2;
    }

    struct spg_journal_reader reader = {};
    enum spg_status status = spg_journal_reader_open(&reader, path);
    if (status != SPG_OK) {
        fprintf(stderr, "verify-journal: open failed: %s\n",
                spg_status_to_string(status));
        return 1;
    }

    uint8_t payload[CLI_JOURNAL_VERIFY_PAYLOAD_BYTES];
    uint64_t counts[SPG_JOURNAL_EVENT_ERROR + 1u] = {};
    uint64_t total = 0u;
    uint64_t truncated_payloads = 0u;
    uint64_t status_failures = 0u;
    uint64_t payload_bytes = 0u;
    uint64_t last_sequence = 0u;
    struct spg_journal_record record = {};

    for (;;) {
        status = spg_journal_reader_next(&reader, sizeof payload, payload,
                                         &record);
        if (status == SPG_E_NOT_FOUND) {
            break;
        }
        if (status != SPG_OK && status != SPG_E_LIMIT) {
            fprintf(stderr, "verify-journal: corrupt at next record: %s\n",
                    spg_status_to_string(status));
            (void)spg_journal_reader_close(&reader);
            return 1;
        }

        total += 1u;
        last_sequence = record.header.sequence;
        if (record.header.event_kind <= SPG_JOURNAL_EVENT_ERROR) {
            counts[record.header.event_kind] += 1u;
        }
        if (record.header.status != (uint32_t)SPG_OK) {
            status_failures += 1u;
        }
        if (record.header.payload_bytes > UINT64_MAX - payload_bytes) {
            payload_bytes = UINT64_MAX;
        } else {
            payload_bytes += record.header.payload_bytes;
        }
        if (status == SPG_E_LIMIT) {
            truncated_payloads += 1u;
        }
    }

    const enum spg_status close_status = spg_journal_reader_close(&reader);
    if (close_status != SPG_OK) {
        fprintf(stderr, "verify-journal: close failed: %s\n",
                spg_status_to_string(close_status));
        return 1;
    }

    printf("journal=%s\n", path);
    printf("verified=true\n");
    printf("records=%llu\n", (unsigned long long)total);
    printf("last_sequence=%llu\n", (unsigned long long)last_sequence);
    printf("payload_bytes=%llu\n", (unsigned long long)payload_bytes);
    printf("status_failures=%llu\n", (unsigned long long)status_failures);
    printf("truncated_payloads=%llu\n",
           (unsigned long long)truncated_payloads);
    for (uint32_t i = 0u; i <= (uint32_t)SPG_JOURNAL_EVENT_ERROR; i += 1u) {
        if (counts[i] == 0u) {
            continue;
        }
        printf("event.%s=%llu\n", journal_event_kind_name(i),
               (unsigned long long)counts[i]);
    }
    return 0;
}

static void print_replay_common_json(const struct spg_journal_record *record,
                                     const bool truncated) {
    printf("{\"seq\":%llu", (unsigned long long)record->header.sequence);
    printf(",\"parent\":%llu",
           (unsigned long long)record->header.parent_sequence);
    printf(",\"ts\":%llu", (unsigned long long)record->header.timestamp_ns);
    printf(",\"event\":");
    print_json_bytes(strlen(journal_event_kind_name(record->header.event_kind)),
                     (const uint8_t *)journal_event_kind_name(
                         record->header.event_kind));
    printf(",\"status\":");
    print_json_bytes(
        strlen(spg_status_to_string((enum spg_status)record->header.status)),
        (const uint8_t *)spg_status_to_string(
            (enum spg_status)record->header.status));
    printf(",\"payload_bytes\":%llu",
           (unsigned long long)record->header.payload_bytes);
    printf(",\"truncated\":%s", truncated ? "true" : "false");
}

static void print_replay_field_json(const size_t payload_n,
                                    const uint8_t payload[],
                                    const char *form_name,
                                    const char *field_name) {
    struct spg_text_span value = {};
    if (!replay_payload_field(payload_n, payload, form_name, field_name,
                              &value)) {
        return;
    }
    printf(",\"%s\":", field_name);
    print_json_span_value(payload_n, payload, value);
}

static void print_replay_policy_json(const size_t payload_n,
                                     const uint8_t payload[]) {
    const char *fields[] = {"decision", "deny_reason", "action_kind",
                            "cost", "uses_network", "confidence_bp"};
    for (size_t i = 0u; i < sizeof fields / sizeof fields[0]; i += 1u) {
        print_replay_field_json(payload_n, payload, "policy_decision",
                                fields[i]);
    }
}

static void print_replay_sim_json(const size_t payload_n,
                                  const uint8_t payload[]) {
    const char *fields[] = {"action", "selected_index", "mutated",
                            "risk_before", "risk_after"};
    for (size_t i = 0u; i < sizeof fields / sizeof fields[0]; i += 1u) {
        print_replay_field_json(payload_n, payload, "sim_result", fields[i]);
    }
}

static int replay_command(const char *path) {
    if (path == nullptr) {
        return 2;
    }

    struct spg_journal_reader reader = {};
    enum spg_status status = spg_journal_reader_open(&reader, path);
    if (status != SPG_OK) {
        fprintf(stderr, "replay: open failed: %s\n",
                spg_status_to_string(status));
        return 1;
    }

    uint8_t payload[CLI_REPLAY_PAYLOAD_BYTES];
    struct spg_journal_record record = {};

    for (;;) {
        status = spg_journal_reader_next(&reader, sizeof payload, payload,
                                         &record);
        if (status == SPG_E_NOT_FOUND) {
            break;
        }
        if (status != SPG_OK && status != SPG_E_LIMIT) {
            fprintf(stderr, "replay: corrupt at next record: %s\n",
                    spg_status_to_string(status));
            (void)spg_journal_reader_close(&reader);
            return 1;
        }

        const bool truncated = status == SPG_E_LIMIT;
        print_replay_common_json(&record, truncated);
        const size_t available_payload =
            truncated ? sizeof payload : record.payload_used;
        switch ((enum spg_journal_event_kind)record.header.event_kind) {
        case SPG_JOURNAL_EVENT_MODEL_OUTPUT:
            printf(",\"preview\":");
            print_json_preview(available_payload, payload);
            printf(",\"preview_truncated\":%s",
                   available_payload > CLI_REPLAY_PREVIEW_BYTES ? "true"
                                                                 : "false");
            break;
        case SPG_JOURNAL_EVENT_POLICY_DECISION:
            if (!truncated) {
                print_replay_policy_json(available_payload, payload);
            }
            break;
        case SPG_JOURNAL_EVENT_SIM:
            if (!truncated) {
                print_replay_sim_json(available_payload, payload);
            }
            break;
        case SPG_JOURNAL_EVENT_RUN_START:
        case SPG_JOURNAL_EVENT_MODEL_INPUT:
        case SPG_JOURNAL_EVENT_ACTION:
        case SPG_JOURNAL_EVENT_RESULT:
        case SPG_JOURNAL_EVENT_GRAPH:
        case SPG_JOURNAL_EVENT_MEMORY:
        case SPG_JOURNAL_EVENT_ERROR:
            break;
        }
        printf("}\n");
    }

    const enum spg_status close_status = spg_journal_reader_close(&reader);
    if (close_status != SPG_OK) {
        fprintf(stderr, "replay: close failed: %s\n",
                spg_status_to_string(close_status));
        return 1;
    }

    return 0;
}

static void print_budget_summary(const struct spg_run_budgets *budgets) {
    printf("budget.inference_steps=%llu\n",
           (unsigned long long)budgets->inference_steps);
    printf("budget.tokens=%llu\n", (unsigned long long)budgets->tokens);
    printf("budget.shell_actions=%llu\n",
           (unsigned long long)budgets->shell_actions);
    printf("budget.sim_actions=%llu\n",
           (unsigned long long)budgets->sim_actions);
    printf("budget.memory_actions=%llu\n",
           (unsigned long long)budgets->memory_actions);
    printf("budget.wall_ms=%llu\n", (unsigned long long)budgets->wall_ms);
    printf("budget.journal_bytes=%llu\n",
           (unsigned long long)budgets->journal_bytes);
    printf("budget.risk_bp=%llu\n", (unsigned long long)budgets->risk_bp);
}

static int policy_check_command(const char *path) {
    if (path == nullptr) {
        return 2;
    }
    struct file_buffer policy_text = {};
    struct spg_policy_config policy = {};
    const enum spg_status status = load_policy_file(path, &policy_text, &policy);
    if (status != SPG_OK) {
        fprintf(stderr, "policy-check: load failed: %s\n",
                spg_status_to_string(status));
        free_file_buffer(&policy_text);
        return 1;
    }

    printf("policy=%s\n", path);
    printf("valid=true\n");
    printf("network_default=%s\n",
           policy_network_default_name(policy.network_default));
    print_budget_summary(&policy.budgets);
    printf("capabilities=%zu\n", policy.capability_count);
    for (size_t i = 0u; i < policy.capability_count; i += 1u) {
        const struct spg_policy_capability *cap = &policy.capabilities[i];
        printf("capability.%zu.name=", i);
        print_span_text(policy_text.n, policy_text.data, cap->name);
        printf("\n");
        printf("capability.%zu.kind=%s\n", i,
               policy_capability_kind_name(cap->kind));
        printf("capability.%zu.enabled=%s\n", i,
               cap->enabled ? "true" : "false");
        printf("capability.%zu.budget=%llu\n", i,
               (unsigned long long)cap->budget);
    }

    free_file_buffer(&policy_text);
    return 0;
}

static void print_risk_summary(const struct spg_risk_score *risk) {
    printf("risk.asset=%llu\n", (unsigned long long)risk->asset_component);
    printf("risk.exposure=%llu\n",
           (unsigned long long)risk->exposure_component);
    printf("risk.vulnerability=%llu\n",
           (unsigned long long)risk->vulnerability_component);
    printf("risk.credential=%llu\n",
           (unsigned long long)risk->credential_component);
    printf("risk.reachability=%llu\n",
           (unsigned long long)risk->reachability_component);
    printf("risk.total=%llu\n", (unsigned long long)risk->total);
}

static bool parse_size(const char *text, size_t *out) {
    if (text == nullptr || out == nullptr || text[0] == '\0') {
        return false;
    }
    errno     = 0;
    char *end = nullptr;
    const unsigned long long value = strtoull(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' ||
        value > (unsigned long long)SIZE_MAX) {
        return false;
    }
    *out = (size_t)value;
    return true;
}

static bool parse_positive_size(const char *text, size_t *out) {
    return parse_size(text, out) && *out > 0u;
}

static void add_budget_u64(uint64_t *value, const uint64_t delta) {
    if (value == nullptr) {
        return;
    }
    if (delta > UINT64_MAX - *value) {
        *value = UINT64_MAX;
        return;
    }
    *value += delta;
}

static uint64_t latest_result_sequence(
    const struct spg_orchestrator_result *result,
    const uint64_t fallback_sequence) {
    if (result == nullptr) {
        return fallback_sequence;
    }
    if (result->sim.memory_sequence != 0u) {
        return result->sim.memory_sequence;
    }
    if (result->sim.graph_sequence != 0u) {
        return result->sim.graph_sequence;
    }
    if (result->sim.sim_sequence != 0u) {
        return result->sim.sim_sequence;
    }
    if (result->policy_gate.policy_sequence != 0u) {
        return result->policy_gate.policy_sequence;
    }
    if (result->actor.memory_sequence != 0u) {
        return result->actor.memory_sequence;
    }
    if (result->actor.graph_sequence != 0u) {
        return result->actor.graph_sequence;
    }
    if (result->actor.model_output_sequence != 0u) {
        return result->actor.model_output_sequence;
    }
    if (result->actor.model_input_sequence != 0u) {
        return result->actor.model_input_sequence;
    }
    return fallback_sequence;
}

static void update_run_usage(struct spg_policy_usage *usage,
                             const struct spg_orchestrator_result *result) {
    if (usage == nullptr || result == nullptr) {
        return;
    }
    add_budget_u64(&usage->consumed.inference_steps, 1u);
    add_budget_u64(&usage->consumed.tokens,
                   (uint64_t)result->actor.tokens_decoded);
    if (spg_orchestrator_sim_executed(result)) {
        add_budget_u64(&usage->consumed.sim_actions,
                       result->recommendation.action.cost);
    }
    if (spg_orchestrator_memory_executed(result)) {
        add_budget_u64(&usage->consumed.memory_actions,
                       result->recommendation.action.cost);
    }
}

static void print_run_tick_summary(
    const size_t tick_index, const struct spg_orchestrator_result *result,
    const struct spg_policy_usage *usage) {
    printf("tick=%zu", tick_index);
    printf(" stage=%s", spg_orchestrator_stage_to_string(result->stage));
    printf(" recommendation=%s",
           spg_orchestrator_recommendation_valid(result) ? "valid" : "rejected");
    if (!spg_orchestrator_recommendation_valid(result)) {
        printf(" reject_reason=%s",
               spg_recommendation_reject_reason_to_string(
                   result->recommendation.reject_reason));
    } else {
        printf(" action=%s",
               spg_action_kind_to_string(result->recommendation.action_kind));
    }
    if (spg_orchestrator_policy_evaluated(result)) {
        printf(" policy=%s",
               result->policy_gate.decision.kind == SPG_POLICY_DECISION_ALLOW
                   ? "allow"
                   : "deny");
        printf(" deny_reason=%s",
               spg_policy_deny_reason_to_string(
                   result->policy_gate.decision.deny_reason));
    }
    if (spg_orchestrator_sim_executed(result)) {
        printf(" sim_action=%s",
               spg_sim_exec_action_to_string(result->sim.action));
        printf(" risk_before=%llu",
               (unsigned long long)result->sim.risk_before.total);
        printf(" risk_after=%llu",
               (unsigned long long)result->sim.risk_after.total);
    }
    printf(" consumed.inference_steps=%llu",
           (unsigned long long)usage->consumed.inference_steps);
    printf(" consumed.tokens=%llu",
           (unsigned long long)usage->consumed.tokens);
    printf(" consumed.sim_actions=%llu",
           (unsigned long long)usage->consumed.sim_actions);
    printf("\n");
}

static bool file_span_valid(const size_t text_n,
                            const struct spg_text_span span) {
    return span.offset <= text_n && span.length <= text_n - span.offset;
}

static enum spg_status write_file_text(FILE *file, const char *text) {
    if (file == nullptr || text == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    return fputs(text, file) >= 0 ? SPG_OK : SPG_E_IO;
}

static enum spg_status write_file_span(FILE *file, const size_t text_n,
                                       const char text[],
                                       const struct spg_text_span span) {
    if (file == nullptr || text == nullptr || !file_span_valid(text_n, span)) {
        return SPG_E_INVALID_ARG;
    }
    if (span.length == 0u) {
        return SPG_OK;
    }
    return fwrite(text + span.offset, 1u, span.length, file) == span.length
               ? SPG_OK
               : SPG_E_IO;
}

static enum spg_status write_file_u32(FILE *file, const uint32_t value) {
    if (file == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    return fprintf(file, "%u", value) >= 0 ? SPG_OK : SPG_E_IO;
}

static enum spg_status write_file_u64(FILE *file, const uint64_t value) {
    if (file == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    return fprintf(file, "%llu", (unsigned long long)value) >= 0 ? SPG_OK
                                                                 : SPG_E_IO;
}

static enum spg_status write_file_bool(FILE *file, const bool value) {
    return write_file_text(file, value ? "true" : "false");
}

static enum spg_status write_sim_state_file(const char *path,
                                            const size_t source_n,
                                            const char source[],
                                            const struct spg_sim_config *sim) {
    if (path == nullptr || source == nullptr || sim == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    FILE *file = fopen(path, "wb");
    if (file == nullptr) {
        return SPG_E_IO;
    }

#define WRITE_TEXT(value_)                                                     \
    do {                                                                       \
        status = write_file_text(file, (value_));                              \
        if (status != SPG_OK) {                                                \
            goto done;                                                         \
        }                                                                      \
    } while (0)
#define WRITE_SPAN(span_)                                                      \
    do {                                                                       \
        status = write_file_span(file, source_n, source, (span_));             \
        if (status != SPG_OK) {                                                \
            goto done;                                                         \
        }                                                                      \
    } while (0)
#define WRITE_U32(value_)                                                      \
    do {                                                                       \
        status = write_file_u32(file, (value_));                               \
        if (status != SPG_OK) {                                                \
            goto done;                                                         \
        }                                                                      \
    } while (0)
#define WRITE_BOOL(value_)                                                     \
    do {                                                                       \
        status = write_file_bool(file, (value_));                              \
        if (status != SPG_OK) {                                                \
            goto done;                                                         \
        }                                                                      \
    } while (0)

    enum spg_status status = SPG_OK;
    WRITE_TEXT("(scenario\n");
    for (size_t i = 0u; i < sim->host_count; i += 1u) {
        WRITE_TEXT(" (host (id ");
        WRITE_SPAN(sim->hosts[i].id);
        WRITE_TEXT(") (criticality_bp ");
        WRITE_U32(sim->hosts[i].criticality_bp);
        WRITE_TEXT("))\n");
    }
    for (size_t i = 0u; i < sim->service_count; i += 1u) {
        const struct spg_sim_service *service = &sim->services[i];
        if (service->host_index >= sim->host_count) {
            status = SPG_E_SCHEMA;
            goto done;
        }
        WRITE_TEXT(" (service (id ");
        WRITE_SPAN(service->id);
        WRITE_TEXT(") (host ");
        WRITE_SPAN(sim->hosts[service->host_index].id);
        WRITE_TEXT(") (name ");
        WRITE_SPAN(service->name);
        WRITE_TEXT(") (port ");
        WRITE_U32(service->port);
        WRITE_TEXT(") (exposure_bp ");
        WRITE_U32(service->exposure_bp);
        WRITE_TEXT("))\n");
    }
    for (size_t i = 0u; i < sim->account_count; i += 1u) {
        const struct spg_sim_account *account = &sim->accounts[i];
        if (account->host_index >= sim->host_count) {
            status = SPG_E_SCHEMA;
            goto done;
        }
        WRITE_TEXT(" (account (id ");
        WRITE_SPAN(account->id);
        WRITE_TEXT(") (host ");
        WRITE_SPAN(sim->hosts[account->host_index].id);
        WRITE_TEXT(") (username ");
        WRITE_SPAN(account->username);
        WRITE_TEXT(") (enabled ");
        WRITE_BOOL(account->enabled);
        WRITE_TEXT("))\n");
    }
    for (size_t i = 0u; i < sim->credential_count; i += 1u) {
        const struct spg_sim_credential *credential = &sim->credentials[i];
        if (credential->account_index >= sim->account_count) {
            status = SPG_E_SCHEMA;
            goto done;
        }
        WRITE_TEXT(" (credential (id ");
        WRITE_SPAN(credential->id);
        WRITE_TEXT(") (account ");
        WRITE_SPAN(sim->accounts[credential->account_index].id);
        WRITE_TEXT(") (strength_bp ");
        WRITE_U32(credential->strength_bp);
        WRITE_TEXT("))\n");
    }
    for (size_t i = 0u; i < sim->vulnerability_count; i += 1u) {
        const struct spg_sim_vulnerability *vuln = &sim->vulnerabilities[i];
        if (vuln->service_index >= sim->service_count) {
            status = SPG_E_SCHEMA;
            goto done;
        }
        WRITE_TEXT(" (vulnerability (id ");
        WRITE_SPAN(vuln->id);
        WRITE_TEXT(") (service ");
        WRITE_SPAN(sim->services[vuln->service_index].id);
        WRITE_TEXT(") (severity_bp ");
        WRITE_U32(vuln->severity_bp);
        WRITE_TEXT(") (patched ");
        WRITE_BOOL(vuln->patched);
        WRITE_TEXT("))\n");
    }
    for (size_t i = 0u; i < sim->network_edge_count; i += 1u) {
        const struct spg_sim_network_edge *edge = &sim->network_edges[i];
        if (edge->from_host_index >= sim->host_count ||
            edge->to_host_index >= sim->host_count) {
            status = SPG_E_SCHEMA;
            goto done;
        }
        WRITE_TEXT(" (network_edge (from ");
        WRITE_SPAN(sim->hosts[edge->from_host_index].id);
        WRITE_TEXT(") (to ");
        WRITE_SPAN(sim->hosts[edge->to_host_index].id);
        WRITE_TEXT(") (reachability_bp ");
        WRITE_U32(edge->reachability_bp);
        WRITE_TEXT("))\n");
    }
    WRITE_TEXT(")\n");

done:
#undef WRITE_BOOL
#undef WRITE_U32
#undef WRITE_SPAN
#undef WRITE_TEXT
    if (fclose(file) != 0 && status == SPG_OK) {
        status = SPG_E_IO;
    }
    return status;
}

static enum spg_status write_run_state_file(
    const char *path, const size_t ticks, const struct spg_graph *graph,
    const struct spg_memory *memory, const struct spg_policy_usage *usage,
    const struct spg_risk_score *risk) {
    if (path == nullptr || graph == nullptr || memory == nullptr ||
        usage == nullptr || risk == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    FILE *file = fopen(path, "wb");
    if (file == nullptr) {
        return SPG_E_IO;
    }

#define WRITE_STATE_TEXT(value_)                                               \
    do {                                                                       \
        status = write_file_text(file, (value_));                              \
        if (status != SPG_OK) {                                                \
            goto done;                                                         \
        }                                                                      \
    } while (0)
#define WRITE_STATE_U64(value_)                                                \
    do {                                                                       \
        status = write_file_u64(file, (value_));                               \
        if (status != SPG_OK) {                                                \
            goto done;                                                         \
        }                                                                      \
    } while (0)

    enum spg_status status = SPG_OK;
    WRITE_STATE_TEXT("{\"version\":1");
    WRITE_STATE_TEXT(",\"ticks\":");
    WRITE_STATE_U64((uint64_t)ticks);
    WRITE_STATE_TEXT(",\"consumed\":{\"inference_steps\":");
    WRITE_STATE_U64(usage->consumed.inference_steps);
    WRITE_STATE_TEXT(",\"tokens\":");
    WRITE_STATE_U64(usage->consumed.tokens);
    WRITE_STATE_TEXT(",\"shell_actions\":");
    WRITE_STATE_U64(usage->consumed.shell_actions);
    WRITE_STATE_TEXT(",\"sim_actions\":");
    WRITE_STATE_U64(usage->consumed.sim_actions);
    WRITE_STATE_TEXT(",\"memory_actions\":");
    WRITE_STATE_U64(usage->consumed.memory_actions);
    WRITE_STATE_TEXT("}");
    WRITE_STATE_TEXT(",\"graph\":{\"nodes\":");
    WRITE_STATE_U64((uint64_t)graph->node_count);
    WRITE_STATE_TEXT(",\"edges\":");
    WRITE_STATE_U64((uint64_t)graph->edge_count);
    WRITE_STATE_TEXT("}");
    WRITE_STATE_TEXT(",\"memory\":{\"facts\":");
    WRITE_STATE_U64((uint64_t)memory->fact_count);
    WRITE_STATE_TEXT("}");
    WRITE_STATE_TEXT(",\"risk\":{\"total\":");
    WRITE_STATE_U64(risk->total);
    WRITE_STATE_TEXT("}}\n");

done:
#undef WRITE_STATE_U64
#undef WRITE_STATE_TEXT
    if (fclose(file) != 0 && status == SPG_OK) {
        status = SPG_E_IO;
    }
    return status;
}

static int sim_validate_command(const char *path) {
    if (path == nullptr) {
        return 2;
    }
    struct file_buffer scenario_text = {};
    struct spg_sim_config sim = {};
    enum spg_status status = load_scenario_file(path, &scenario_text, &sim);
    if (status != SPG_OK) {
        fprintf(stderr, "sim-validate: load failed: %s\n",
                spg_status_to_string(status));
        free_file_buffer(&scenario_text);
        return 1;
    }
    struct spg_risk_score risk = {};
    status = spg_risk_evaluate(&sim, &risk);
    if (status != SPG_OK) {
        fprintf(stderr, "sim-validate: risk failed: %s\n",
                spg_status_to_string(status));
        free_file_buffer(&scenario_text);
        return 1;
    }

    printf("scenario=%s\n", path);
    printf("valid=true\n");
    printf("hosts=%zu\n", sim.host_count);
    printf("services=%zu\n", sim.service_count);
    printf("accounts=%zu\n", sim.account_count);
    printf("credentials=%zu\n", sim.credential_count);
    printf("vulnerabilities=%zu\n", sim.vulnerability_count);
    printf("network_edges=%zu\n", sim.network_edge_count);
    print_risk_summary(&risk);

    free_file_buffer(&scenario_text);
    return 0;
}

static void free_file_buffer(struct file_buffer *buffer) {
    if (buffer == nullptr) {
        return;
    }
    safe_free((void **)&buffer->data);
    buffer->n = 0u;
}

static enum spg_status read_file(const char *path, struct file_buffer *out) {
    if (path == nullptr || out == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out = (struct file_buffer){};
    FILE *file = fopen(path, "rb");
    if (file == nullptr) {
        return SPG_E_IO;
    }
    if (fseek(file, 0L, SEEK_END) != 0) {
        (void)fclose(file);
        return SPG_E_IO;
    }
    const long end = ftell(file);
    if (end < 0) {
        (void)fclose(file);
        return SPG_E_IO;
    }
    if ((unsigned long)end > SIZE_MAX - 1u) {
        (void)fclose(file);
        return SPG_E_OVERFLOW;
    }
    if (fseek(file, 0L, SEEK_SET) != 0) {
        (void)fclose(file);
        return SPG_E_IO;
    }
    const size_t n = (size_t)end;
    char *data = heap_alloc_aligned(n + 1u, alignof(char));
    if (data == nullptr) {
        (void)fclose(file);
        return SPG_E_OOM;
    }
    if (n > 0u && fread(data, 1u, n, file) != n) {
        safe_free((void **)&data);
        (void)fclose(file);
        return SPG_E_IO;
    }
    if (fclose(file) != 0) {
        safe_free((void **)&data);
        return SPG_E_IO;
    }
    data[n] = '\0';
    out->n = n;
    out->data = data;
    return SPG_OK;
}

static enum spg_status span_to_cstr(const size_t input_n, const char input[],
                                    const struct spg_text_span span,
                                    char **out) {
    if (input == nullptr || out == nullptr || span.offset > input_n ||
        span.length > input_n - span.offset) {
        return SPG_E_INVALID_ARG;
    }
    *out = nullptr;
    char *text = heap_alloc_aligned(span.length + 1u, alignof(char));
    if (text == nullptr) {
        return SPG_E_OOM;
    }
    memcpy(text, input + span.offset, span.length);
    text[span.length] = '\0';
    *out = text;
    return SPG_OK;
}

static enum spg_status load_run_file(const char *path,
                                     struct file_buffer *run_text,
                                     struct spg_run_config *run) {
    enum spg_status status = read_file(path, run_text);
    if (status != SPG_OK) {
        return status;
    }
    struct spg_sexpr_token tokens[CLI_TOKEN_CAPACITY];
    struct spg_sexpr_node  nodes[CLI_NODE_CAPACITY];
    struct spg_run_config_error error = {};
    status = spg_run_config_load(run_text->n, run_text->data,
                                 CLI_TOKEN_CAPACITY, tokens,
                                 CLI_NODE_CAPACITY, nodes, run, &error);
    return status;
}

static enum spg_status load_policy_file(const char *path,
                                        struct file_buffer *policy_text,
                                        struct spg_policy_config *policy) {
    enum spg_status status = read_file(path, policy_text);
    if (status != SPG_OK) {
        return status;
    }
    struct spg_sexpr_token tokens[CLI_TOKEN_CAPACITY];
    struct spg_sexpr_node  nodes[CLI_NODE_CAPACITY];
    struct spg_policy_config_error error = {};
    status = spg_policy_config_load(policy_text->n, policy_text->data,
                                    CLI_TOKEN_CAPACITY, tokens,
                                    CLI_NODE_CAPACITY, nodes, policy, &error);
    return status;
}

static enum spg_status load_scenario_file(const char *path,
                                          struct file_buffer *scenario_text,
                                          struct spg_sim_config *sim) {
    enum spg_status status = read_file(path, scenario_text);
    if (status != SPG_OK) {
        return status;
    }
    struct spg_sexpr_token tokens[CLI_TOKEN_CAPACITY];
    struct spg_sexpr_node  nodes[CLI_NODE_CAPACITY];
    struct spg_sim_config_error error = {};
    status = spg_sim_config_load(scenario_text->n, scenario_text->data,
                                 CLI_TOKEN_CAPACITY, tokens,
                                 CLI_NODE_CAPACITY, nodes, sim, &error);
    return status;
}

static int run_tick_fake(const char *run_path, const char *fake_output) {
    if (run_path == nullptr || fake_output == nullptr) {
        return 2;
    }

    int rc = 1;
    struct file_buffer run_text = {};
    struct file_buffer policy_text = {};
    struct file_buffer scenario_text = {};
    char *policy_path = nullptr;
    char *scenario_path = nullptr;
    char *journal_path = nullptr;
    struct spg_journal_writer journal = {};
    bool journal_open = false;
    struct spg_model_adapter model = {};
    bool model_open = false;

    struct spg_run_config run = {};
    enum spg_status status = load_run_file(run_path, &run_text, &run);
    if (status != SPG_OK) {
        fprintf(stderr, "tick: load run failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }
    status = span_to_cstr(run_text.n, run_text.data, run.policy_path,
                          &policy_path);
    if (status != SPG_OK) {
        fprintf(stderr, "tick: policy path failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }
    status = span_to_cstr(run_text.n, run_text.data, run.scenario_path,
                          &scenario_path);
    if (status != SPG_OK) {
        fprintf(stderr, "tick: scenario path failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }
    status = span_to_cstr(run_text.n, run_text.data, run.journal_path,
                          &journal_path);
    if (status != SPG_OK) {
        fprintf(stderr, "tick: journal path failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }

    struct spg_policy_config policy = {};
    status = load_policy_file(policy_path, &policy_text, &policy);
    if (status != SPG_OK) {
        fprintf(stderr, "tick: load policy failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }
    struct spg_sim_config sim = {};
    status = load_scenario_file(scenario_path, &scenario_text, &sim);
    if (status != SPG_OK) {
        fprintf(stderr, "tick: load scenario failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }

    status = spg_journal_writer_open(&journal, journal_path);
    if (status != SPG_OK) {
        fprintf(stderr, "tick: open journal failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }
    journal_open = true;

    const struct spg_model_adapter_config model_config = {
        .kind            = SPG_MODEL_ADAPTER_FAKE,
        .sampling        = {.top_p = 1.0f, .random_seed = run.seed},
        .fake_response_n = strlen(fake_output),
        .fake_response   = fake_output,
    };
    status = spg_model_adapter_init(&model, &model_config);
    if (status != SPG_OK) {
        fprintf(stderr, "tick: fake model init failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }
    model_open = true;

    struct spg_graph graph = {};
    struct spg_memory memory = {};
    spg_graph_init(&graph);
    spg_memory_init(&memory);

    struct spg_context_graph_ref graph_refs[CLI_CONTEXT_REFS];
    struct spg_context_memory_ref memory_refs[CLI_CONTEXT_REFS];
    struct spg_context_journal_ref journal_refs[CLI_CONTEXT_REFS];
    char context[CLI_CONTEXT_BYTES];
    char model_output[CLI_MODEL_OUTPUT_BYTES];
    struct spg_sexpr_token rec_tokens[CLI_TOKEN_CAPACITY];
    struct spg_sexpr_node rec_nodes[CLI_NODE_CAPACITY];
    char policy_payload[CLI_PAYLOAD_BYTES];
    char sim_payload[CLI_PAYLOAD_BYTES];

    const struct spg_orchestrator_workspace workspace = {
        .actor = {
            .context_capacity      = sizeof context,
            .context               = context,
            .model_output_capacity = sizeof model_output,
            .model_output          = model_output,
            .graph_ref_capacity    = CLI_CONTEXT_REFS,
            .graph_refs            = graph_refs,
            .memory_ref_capacity   = CLI_CONTEXT_REFS,
            .memory_refs           = memory_refs,
            .journal_ref_capacity  = CLI_CONTEXT_REFS,
            .journal_refs          = journal_refs,
        },
        .recommendation_token_capacity = CLI_TOKEN_CAPACITY,
        .recommendation_tokens         = rec_tokens,
        .recommendation_node_capacity  = CLI_NODE_CAPACITY,
        .recommendation_nodes          = rec_nodes,
        .policy_payload_capacity       = sizeof policy_payload,
        .policy_payload                = policy_payload,
        .sim_payload_capacity          = sizeof sim_payload,
        .sim_payload                   = sim_payload,
    };

    struct spg_policy_usage usage = {};
    struct spg_orchestrator_state state = {
        .graph         = &graph,
        .memory        = &memory,
        .journal       = &journal,
        .model         = &model,
        .sim           = &sim,
        .run           = &run,
        .usage         = &usage,
        .policy        = &policy,
        .policy_text_n = policy_text.n,
        .policy_text   = policy_text.data,
        .graph_text_n  = CLI_CONTEXT_BYTES,
        .graph_text    = context,
        .memory_text_n = CLI_MODEL_OUTPUT_BYTES,
        .memory_text   = model_output,
    };
    const struct spg_orchestrator_config config = {
        .actor_id            = 1u,
        .timestamp_ns        = 1u,
        .context_limits      = {.graph_nodes = CLI_CONTEXT_REFS,
                                .memory_facts = CLI_CONTEXT_REFS,
                                .journal_events = CLI_CONTEXT_REFS},
        .max_decode_tokens   = run.budgets.tokens > 0u ? 1u : 0u,
        .reset_model_session = true,
        .write_journal       = true,
        .update_graph        = true,
        .update_memory       = true,
    };
    struct spg_orchestrator_result result = {};
    status = spg_orchestrator_tick(&state, &config, &workspace, &result);
    if (status != SPG_OK) {
        fprintf(stderr, "tick: orchestrator failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }

    printf("stage=%s\n", spg_orchestrator_stage_to_string(result.stage));
    printf("recommendation=%s\n",
           spg_orchestrator_recommendation_valid(&result) ? "valid"
                                                          : "rejected");
    if (!spg_orchestrator_recommendation_valid(&result)) {
        printf("reject_reason=%s\n",
               spg_recommendation_reject_reason_to_string(
                   result.recommendation.reject_reason));
    }
    if (spg_orchestrator_policy_evaluated(&result)) {
        printf("policy=%s\n",
               result.policy_gate.decision.kind == SPG_POLICY_DECISION_ALLOW
                   ? "allow"
                   : "deny");
        printf("deny_reason=%s\n",
               spg_policy_deny_reason_to_string(
                   result.policy_gate.decision.deny_reason));
    }
    if (spg_orchestrator_sim_executed(&result)) {
        printf("sim_action=%s\n",
               spg_sim_exec_action_to_string(result.sim.action));
        printf("risk_before=%llu\n",
               (unsigned long long)result.sim.risk_before.total);
        printf("risk_after=%llu\n",
               (unsigned long long)result.sim.risk_after.total);
    }
    printf("journal=%s\n", journal_path);
    printf("graph_nodes=%zu\n", graph.node_count);
    printf("memory_facts=%zu\n", memory.fact_count);
    rc = 0;

done:
    if (journal_open) {
        const enum spg_status close_status = spg_journal_writer_close(&journal);
        if (close_status != SPG_OK && rc == 0) {
            fprintf(stderr, "tick: close journal failed: %s\n",
                    spg_status_to_string(close_status));
            rc = 1;
        }
    }
    if (model_open) {
        spg_model_adapter_destroy(&model);
    }
    safe_free((void **)&policy_path);
    safe_free((void **)&scenario_path);
    safe_free((void **)&journal_path);
    free_file_buffer(&run_text);
    free_file_buffer(&policy_text);
    free_file_buffer(&scenario_text);
    return rc;
}

static int run_loop(const char *run_path, const char *fake_output,
                    const size_t ticks, const char *sim_state_path,
                    const char *run_state_path, const char *memory_dir,
                    const char *remote_url, const char *remote_model) {
    if (run_path == nullptr || ticks == 0u) {
        return 2;
    }

    /* Memory store is opt-in: only opened when a directory is configured, so a
     * plain run never creates one. */
    struct spg_mem_store mem_store_obj;
    bool                 have_store = false;
    if (memory_dir != nullptr && memory_dir[0] != '\0') {
        have_store = spg_mem_store_open(&mem_store_obj, memory_dir) == SPG_OK;
        if (!have_store) {
            fprintf(stderr, "run: cannot open memory dir %s\n", memory_dir);
            return 1;
        }
    }

    int rc = 1;
    struct file_buffer run_text = {};
    struct file_buffer policy_text = {};
    struct file_buffer scenario_text = {};
    char *policy_path = nullptr;
    char *scenario_path = nullptr;
    char *journal_path = nullptr;
    char *model_path = nullptr;
    struct spg_journal_writer journal = {};
    bool journal_open = false;
    struct spg_model_adapter model = {};
    bool model_open = false;

    struct spg_run_config run = {};
    enum spg_status status = load_run_file(run_path, &run_text, &run);
    if (status != SPG_OK) {
        fprintf(stderr, "run: load run failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }
    if (run.budgets.inference_steps > 0u &&
        ticks > (size_t)run.budgets.inference_steps) {
        fprintf(stderr,
                "run: ticks exceed budget.inference_steps "
                "(ticks=%zu budget=%llu)\n",
                ticks, (unsigned long long)run.budgets.inference_steps);
        goto done;
    }

    status = span_to_cstr(run_text.n, run_text.data, run.policy_path,
                          &policy_path);
    if (status != SPG_OK) {
        fprintf(stderr, "run: policy path failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }
    status = span_to_cstr(run_text.n, run_text.data, run.scenario_path,
                          &scenario_path);
    if (status != SPG_OK) {
        fprintf(stderr, "run: scenario path failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }
    status = span_to_cstr(run_text.n, run_text.data, run.journal_path,
                          &journal_path);
    if (status != SPG_OK) {
        fprintf(stderr, "run: journal path failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }
    status = span_to_cstr(run_text.n, run_text.data, run.model_path,
                          &model_path);
    if (status != SPG_OK) {
        fprintf(stderr, "run: model path failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }

    struct spg_policy_config policy = {};
    status = load_policy_file(policy_path, &policy_text, &policy);
    if (status != SPG_OK) {
        fprintf(stderr, "run: load policy failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }
    struct spg_sim_config sim = {};
    status = load_scenario_file(scenario_path, &scenario_text, &sim);
    if (status != SPG_OK) {
        fprintf(stderr, "run: load scenario failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }

    status = spg_journal_writer_open(&journal, journal_path);
    if (status != SPG_OK) {
        fprintf(stderr, "run: open journal failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }
    journal_open = true;

    /* Adapter selection: --fake wins; else a remote endpoint (flag or
     * GEISTSHELL_API_URL) selects the REMOTE model; else local GEIST. */
    const bool  use_fake    = fake_output != nullptr;
    const char *env_api_url = getenv("GEISTSHELL_API_URL");
    const char *url =
        (remote_url != nullptr && remote_url[0] != '\0')        ? remote_url
        : (env_api_url != nullptr && env_api_url[0] != '\0')    ? env_api_url
                                                                : nullptr;
    const bool use_remote = !use_fake && url != nullptr;
    /* For REMOTE the (model "...") value is a model name, not a file path; an
     * explicit --remote-model overrides it. The key is env-only. */
    const char *remote_name =
        (remote_model != nullptr && remote_model[0] != '\0') ? remote_model
                                                             : model_path;
    const enum spg_model_adapter_kind kind =
        use_fake     ? SPG_MODEL_ADAPTER_FAKE
        : use_remote ? SPG_MODEL_ADAPTER_REMOTE
                     : SPG_MODEL_ADAPTER_GEIST;
    const struct spg_model_adapter_config model_config = {
        .kind         = kind,
        .model_path   = (kind == SPG_MODEL_ADAPTER_GEIST) ? model_path : nullptr,
        .endpoint_url = use_remote ? url : nullptr,
        .model_name   = use_remote ? remote_name : nullptr,
        .api_key      = use_remote ? getenv("GEISTSHELL_API_KEY") : nullptr,
        .sampling        = {.max_seq_len = 4096u,
                            .temperature = 0.0f,
                            .top_p = 1.0f,
                            .top_k = 0,
                            .random_seed = run.seed},
        .fake_response_n = use_fake ? strlen(fake_output) : 0u,
        .fake_response   = fake_output,
    };
    status = spg_model_adapter_init(&model, &model_config);
    if (status != SPG_OK) {
        fprintf(stderr, "run: model init failed: %s\n",
                spg_status_to_string(status));
        if (use_remote && status == SPG_E_UNSUPPORTED) {
            fprintf(stderr,
                    "run: rebuild with `make REMOTE=1` to enable --remote-url\n");
        }
        goto done;
    }
    model_open = true;

    struct spg_graph graph = {};
    struct spg_memory memory = {};
    spg_graph_init(&graph);
    spg_memory_init(&memory);

    struct spg_context_graph_ref graph_refs[CLI_CONTEXT_REFS];
    struct spg_context_memory_ref memory_refs[CLI_CONTEXT_REFS];
    struct spg_context_journal_ref journal_refs[CLI_CONTEXT_REFS];
    char context[CLI_CONTEXT_BYTES];
    char model_output[CLI_MODEL_OUTPUT_BYTES];
    struct spg_sexpr_token rec_tokens[CLI_TOKEN_CAPACITY];
    struct spg_sexpr_node rec_nodes[CLI_NODE_CAPACITY];
    char policy_payload[CLI_PAYLOAD_BYTES];
    char sim_payload[CLI_PAYLOAD_BYTES];
    char mem_recall_buf[8192] = {0};

    const struct spg_orchestrator_workspace workspace = {
        .actor = {
            .context_capacity      = sizeof context,
            .context               = context,
            .model_output_capacity = sizeof model_output,
            .model_output          = model_output,
            .graph_ref_capacity    = CLI_CONTEXT_REFS,
            .graph_refs            = graph_refs,
            .memory_ref_capacity   = CLI_CONTEXT_REFS,
            .memory_refs           = memory_refs,
            .journal_ref_capacity  = CLI_CONTEXT_REFS,
            .journal_refs          = journal_refs,
        },
        .recommendation_token_capacity = CLI_TOKEN_CAPACITY,
        .recommendation_tokens         = rec_tokens,
        .recommendation_node_capacity  = CLI_NODE_CAPACITY,
        .recommendation_nodes          = rec_nodes,
        .policy_payload_capacity       = sizeof policy_payload,
        .policy_payload                = policy_payload,
        .sim_payload_capacity          = sizeof sim_payload,
        .sim_payload                   = sim_payload,
        .observation_capacity        = sizeof mem_recall_buf,
        .observation_buf             = mem_recall_buf,
    };

    struct spg_policy_usage usage = {};
    char                    mem_index_buf[4096] = {0};
    struct spg_orchestrator_state state = {
        .graph         = &graph,
        .memory        = &memory,
        .journal       = &journal,
        .model         = &model,
        .sim           = &sim,
        .store         = have_store ? &mem_store_obj : nullptr,
        .run           = &run,
        .usage         = &usage,
        .policy        = &policy,
        .policy_text_n = policy_text.n,
        .policy_text   = policy_text.data,
        .graph_text_n  = 0u,
        .graph_text    = nullptr,
        .memory_text_n = 0u,
        .memory_text   = nullptr,
        .memory_index  = have_store ? mem_index_buf : nullptr,
        .observation = have_store ? mem_recall_buf : nullptr,
    };

    uint64_t parent_sequence = 0u;
    for (size_t i = 0u; i < ticks; i += 1u) {
        /* Refresh the memory index so the context reflects saves from prior
         * ticks. */
        if (have_store) {
            (void)spg_mem_index(&mem_store_obj, sizeof mem_index_buf,
                                mem_index_buf, nullptr, nullptr);
        }
        if (run.budgets.tokens > 0u &&
            usage.consumed.tokens >= run.budgets.tokens) {
            fprintf(stderr, "run: token budget exhausted before tick %zu\n",
                    i + 1u);
            goto done;
        }

        const struct spg_orchestrator_config config = {
            .actor_id            = 1u,
            .timestamp_ns        = i + 1u,
            .parent_sequence     = parent_sequence,
            .context_limits      = {.graph_nodes = CLI_CONTEXT_REFS,
                                    .memory_facts = CLI_CONTEXT_REFS,
                                    .journal_events = CLI_CONTEXT_REFS},
            .max_decode_tokens   = run.budgets.tokens > 0u ? 1u : 0u,
            .reset_model_session = true,
            .write_journal       = true,
            .update_graph        = true,
            .update_memory       = true,
        };
        struct spg_orchestrator_result result = {};
        status = spg_orchestrator_tick(&state, &config, &workspace, &result);
        if (status != SPG_OK) {
            fprintf(stderr, "run: orchestrator failed at tick %zu: %s\n",
                    i + 1u, spg_status_to_string(status));
            goto done;
        }

        update_run_usage(&usage, &result);
        parent_sequence = latest_result_sequence(&result, parent_sequence);
        print_run_tick_summary(i + 1u, &result, &usage);
    }

    struct spg_risk_score final_risk = {};
    status = spg_risk_evaluate(&sim, &final_risk);
    if (status != SPG_OK) {
        fprintf(stderr, "run: final risk failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }

    printf("journal=%s\n", journal_path);
    if (sim_state_path != nullptr) {
        status = write_sim_state_file(sim_state_path, scenario_text.n,
                                      scenario_text.data, &sim);
        if (status != SPG_OK) {
            fprintf(stderr, "run: write sim state failed: %s\n",
                    spg_status_to_string(status));
            goto done;
        }
        printf("sim_state=%s\n", sim_state_path);
    }
    if (run_state_path != nullptr) {
        status = write_run_state_file(run_state_path, ticks, &graph, &memory,
                                      &usage, &final_risk);
        if (status != SPG_OK) {
            fprintf(stderr, "run: write run state failed: %s\n",
                    spg_status_to_string(status));
            goto done;
        }
        printf("run_state=%s\n", run_state_path);
    }
    printf("ticks=%zu\n", ticks);
    printf("graph_nodes=%zu\n", graph.node_count);
    printf("memory_facts=%zu\n", memory.fact_count);
    printf("risk_final=%llu\n", (unsigned long long)final_risk.total);
    rc = 0;

done:
    if (journal_open) {
        const enum spg_status close_status = spg_journal_writer_close(&journal);
        if (close_status != SPG_OK && rc == 0) {
            fprintf(stderr, "run: close journal failed: %s\n",
                    spg_status_to_string(close_status));
            rc = 1;
        }
    }
    if (model_open) {
        spg_model_adapter_destroy(&model);
    }
    safe_free((void **)&policy_path);
    safe_free((void **)&scenario_path);
    safe_free((void **)&journal_path);
    safe_free((void **)&model_path);
    free_file_buffer(&run_text);
    free_file_buffer(&policy_text);
    free_file_buffer(&scenario_text);
    return rc;
}

static int tick_command(int argc, char **argv) {
    const char *run_path = nullptr;
    const char *fake_output = nullptr;
    for (int i = 2; i < argc; i += 1) {
        if (strcmp(argv[i], "--run") == 0 && i + 1 < argc) {
            run_path = argv[i + 1];
            i += 1;
            continue;
        }
        if (strcmp(argv[i], "--fake") == 0 && i + 1 < argc) {
            fake_output = argv[i + 1];
            i += 1;
            continue;
        }
        fprintf(stderr, "tick: unknown or incomplete argument: %s\n", argv[i]);
        return 2;
    }
    if (run_path == nullptr || fake_output == nullptr) {
        return 2;
    }
    return run_tick_fake(run_path, fake_output);
}

static int run_command(int argc, char **argv) {
    const char *run_path = nullptr;
    const char *fake_output = nullptr;
    const char *sim_state_path = nullptr;
    const char *run_state_path = nullptr;
    const char *memory_dir     = getenv("GEISTSHELL_MEMORY_DIR");
    const char *remote_url     = nullptr;
    const char *remote_model   = nullptr;
    size_t ticks = 3u;
    for (int i = 2; i < argc; i += 1) {
        if ((strcmp(argv[i], "--config") == 0 ||
             strcmp(argv[i], "--run") == 0) &&
            i + 1 < argc) {
            run_path = argv[i + 1];
            i += 1;
            continue;
        }
        if (strcmp(argv[i], "--fake") == 0 && i + 1 < argc) {
            fake_output = argv[i + 1];
            i += 1;
            continue;
        }
        if (strcmp(argv[i], "--ticks") == 0 && i + 1 < argc) {
            if (!parse_positive_size(argv[i + 1], &ticks)) {
                fprintf(stderr, "run: invalid --ticks value: %s\n",
                        argv[i + 1]);
                return 2;
            }
            i += 1;
            continue;
        }
        if (strcmp(argv[i], "--write-sim-state") == 0 && i + 1 < argc) {
            sim_state_path = argv[i + 1];
            i += 1;
            continue;
        }
        if (strcmp(argv[i], "--write-run-state") == 0 && i + 1 < argc) {
            run_state_path = argv[i + 1];
            i += 1;
            continue;
        }
        if (strcmp(argv[i], "--memory-dir") == 0 && i + 1 < argc) {
            memory_dir = argv[i + 1];
            i += 1;
            continue;
        }
        if (strcmp(argv[i], "--remote-url") == 0 && i + 1 < argc) {
            remote_url = argv[i + 1];
            i += 1;
            continue;
        }
        if (strcmp(argv[i], "--remote-model") == 0 && i + 1 < argc) {
            remote_model = argv[i + 1];
            i += 1;
            continue;
        }
        fprintf(stderr, "run: unknown or incomplete argument: %s\n", argv[i]);
        return 2;
    }
    if (run_path == nullptr) {
        return 2;
    }
    return run_loop(run_path, fake_output, ticks, sim_state_path,
                    run_state_path, memory_dir, remote_url, remote_model);
}

#define AGENT_MAX_SCRIPT   64u
#define AGENT_SHELL_STDOUT 4096u
#define AGENT_SHELL_STDERR 1024u
#define AGENT_OBS_BYTES    8192u
#define CLI_PATH_MAX       4096u

/* Split a script buffer into one fake reply per non-blank line (pointers into
 * data; no copy, no NUL needed). Returns the number of replies. */
static size_t split_script_lines(char *data, const size_t n,
                                 struct spg_fake_response out[static 1],
                                 const size_t cap) {
    size_t count = 0u;
    size_t i     = 0u;
    while (i < n && count < cap) {
        while (i < n && (data[i] == '\n' || data[i] == '\r')) {
            i += 1u;
        }
        if (i >= n) {
            break;
        }
        const size_t start = i;
        while (i < n && data[i] != '\n' && data[i] != '\r') {
            i += 1u;
        }
        out[count].text = data + start;
        out[count].n    = i - start;
        count += 1u;
    }
    return count;
}

/* Governed multi-step agent loop driven by a scripted fake model: each line of
 * --fake-script is one recommendation; the loop gates, executes, journals, and
 * feeds each result forward until `finish` or a termination condition. */
static int agent_command(int argc, char **argv) {
    const char *run_path    = nullptr;
    const char *script_path = nullptr;
    const char *memory_dir  = getenv("GEISTSHELL_MEMORY_DIR");
    size_t      max_steps   = 8u;
    size_t      max_repairs = 2u;
    bool        allow_exec  = false;
    for (int i = 2; i < argc; i += 1) {
        if ((strcmp(argv[i], "--config") == 0 ||
             strcmp(argv[i], "--run") == 0) &&
            i + 1 < argc) {
            run_path = argv[i + 1];
            i += 1;
            continue;
        }
        if (strcmp(argv[i], "--fake-script") == 0 && i + 1 < argc) {
            script_path = argv[i + 1];
            i += 1;
            continue;
        }
        if (strcmp(argv[i], "--max-steps") == 0 && i + 1 < argc) {
            if (!parse_positive_size(argv[i + 1], &max_steps)) {
                fprintf(stderr, "agent: invalid --max-steps value: %s\n",
                        argv[i + 1]);
                return 2;
            }
            i += 1;
            continue;
        }
        if (strcmp(argv[i], "--max-repairs") == 0 && i + 1 < argc) {
            if (!parse_size(argv[i + 1], &max_repairs)) {
                fprintf(stderr, "agent: invalid --max-repairs value: %s\n",
                        argv[i + 1]);
                return 2;
            }
            i += 1;
            continue;
        }
        if (strcmp(argv[i], "--memory-dir") == 0 && i + 1 < argc) {
            memory_dir = argv[i + 1];
            i += 1;
            continue;
        }
        if (strcmp(argv[i], "--allow-exec") == 0) {
            allow_exec = true;
            continue;
        }
        fprintf(stderr, "agent: unknown or incomplete argument: %s\n", argv[i]);
        return 2;
    }
    if (run_path == nullptr || script_path == nullptr) {
        fprintf(stderr,
                "usage: %s agent --config <run> --fake-script <file> "
                "[--max-steps N] [--max-repairs N] [--allow-exec] "
                "[--memory-dir <d>]\n",
                argv[0]);
        return 2;
    }

    int                rc            = 1;
    struct file_buffer run_text      = {};
    struct file_buffer policy_text   = {};
    struct file_buffer scenario_text = {};
    struct file_buffer script_text   = {};
    char              *policy_path   = nullptr;
    char              *scenario_path = nullptr;
    char              *journal_path  = nullptr;
    char              *model_path    = nullptr;
    struct spg_journal_writer journal     = {};
    bool                      journal_open = false;
    struct spg_model_adapter  model        = {};
    bool                      model_open   = false;
    struct spg_mem_store      store        = {};
    bool                      store_open   = false;

    struct spg_run_config run    = {};
    enum spg_status       status = load_run_file(run_path, &run_text, &run);
    if (status != SPG_OK) {
        fprintf(stderr, "agent: load run failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }
    status =
        span_to_cstr(run_text.n, run_text.data, run.policy_path, &policy_path);
    if (status == SPG_OK) {
        status = span_to_cstr(run_text.n, run_text.data, run.scenario_path,
                              &scenario_path);
    }
    if (status == SPG_OK) {
        status = span_to_cstr(run_text.n, run_text.data, run.journal_path,
                              &journal_path);
    }
    if (status != SPG_OK) {
        fprintf(stderr, "agent: run paths failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }
    (void)span_to_cstr(run_text.n, run_text.data, run.model_path, &model_path);

    struct spg_policy_config policy = {};
    status = load_policy_file(policy_path, &policy_text, &policy);
    if (status != SPG_OK) {
        fprintf(stderr, "agent: load policy failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }
    struct spg_sim_config sim = {};
    status = load_scenario_file(scenario_path, &scenario_text, &sim);
    if (status != SPG_OK) {
        fprintf(stderr, "agent: load scenario failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }

    status = read_file(script_path, &script_text);
    if (status != SPG_OK) {
        fprintf(stderr, "agent: read script failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }
    static struct spg_fake_response script[AGENT_MAX_SCRIPT];
    const size_t                    script_n =
        split_script_lines(script_text.data, script_text.n, script,
                           AGENT_MAX_SCRIPT);
    if (script_n == 0u) {
        fprintf(stderr, "agent: empty --fake-script\n");
        goto done;
    }

    status = spg_journal_writer_open(&journal, journal_path);
    if (status != SPG_OK) {
        fprintf(stderr, "agent: open journal failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }
    journal_open = true;

    const struct spg_model_adapter_config model_config = {
        .kind                = SPG_MODEL_ADAPTER_FAKE,
        .sampling            = {.top_p = 1.0f},
        .fake_response_count = script_n,
        .fake_responses      = script,
    };
    status = spg_model_adapter_init(&model, &model_config);
    if (status != SPG_OK) {
        fprintf(stderr, "agent: model init failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }
    model_open = true;

    if (memory_dir != nullptr && memory_dir[0] != '\0') {
        if (spg_mem_store_open(&store, memory_dir) != SPG_OK) {
            fprintf(stderr, "agent: cannot open memory dir %s\n", memory_dir);
            goto done;
        }
        store_open = true;
    }

    static struct spg_context_graph_ref   graph_refs[CLI_CONTEXT_REFS];
    static struct spg_context_memory_ref  memory_refs[CLI_CONTEXT_REFS];
    static struct spg_context_journal_ref journal_refs[CLI_CONTEXT_REFS];
    static char            context[CLI_CONTEXT_BYTES];
    static char            model_output[CLI_MODEL_OUTPUT_BYTES];
    static struct spg_sexpr_token rec_tokens[CLI_TOKEN_CAPACITY];
    static struct spg_sexpr_node  rec_nodes[CLI_NODE_CAPACITY];
    static char            policy_payload[CLI_PAYLOAD_BYTES];
    static char            sim_payload[CLI_PAYLOAD_BYTES];
    static char            observation[AGENT_OBS_BYTES];
    static char            shell_stdout[AGENT_SHELL_STDOUT];
    static char            shell_stderr[AGENT_SHELL_STDERR];
    static char            mem_index[AGENT_OBS_BYTES];
    static struct spg_journal_record_header trajectory[256];

    const struct spg_agent_run_workspace ws = {
        .context_capacity        = sizeof context,
        .context                 = context,
        .model_output_capacity   = sizeof model_output,
        .model_output            = model_output,
        .graph_ref_capacity      = CLI_CONTEXT_REFS,
        .graph_refs              = graph_refs,
        .memory_ref_capacity     = CLI_CONTEXT_REFS,
        .memory_refs             = memory_refs,
        .journal_ref_capacity    = CLI_CONTEXT_REFS,
        .journal_refs            = journal_refs,
        .token_capacity          = CLI_TOKEN_CAPACITY,
        .tokens                  = rec_tokens,
        .node_capacity           = CLI_NODE_CAPACITY,
        .nodes                   = rec_nodes,
        .policy_payload_capacity = sizeof policy_payload,
        .policy_payload          = policy_payload,
        .sim_payload_capacity    = sizeof sim_payload,
        .sim_payload             = sim_payload,
        .observation_capacity    = sizeof observation,
        .observation             = observation,
        .shell_stdout_capacity   = sizeof shell_stdout,
        .shell_stdout            = shell_stdout,
        .shell_stderr_capacity   = sizeof shell_stderr,
        .shell_stderr            = shell_stderr,
        .trajectory_capacity     = sizeof trajectory / sizeof trajectory[0],
        .trajectory              = trajectory,
        .memory_index_capacity   = sizeof mem_index,
        .memory_index            = mem_index,
    };
    const struct spg_agent_run_inputs inputs = {
        .model         = &model,
        .policy        = &policy,
        .policy_text_n = policy_text.n,
        .policy_text   = policy_text.data,
        .run           = &run,
        .sim           = &sim,
        .store         = store_open ? &store : nullptr,
        .journal       = &journal,
    };
    const struct spg_agent_run_config rcfg = {
        .max_steps         = max_steps,
        .max_repairs       = max_repairs,
        .execution_enabled = allow_exec,
        .exec_timeout_ms   = 5000u,
        .exec_stdout_cap   = sizeof shell_stdout,
        .exec_stderr_cap   = sizeof shell_stderr,
        .context_refs      = CLI_CONTEXT_REFS,
    };
    struct spg_policy_usage      usage       = {};
    struct spg_agent_loop_result loop_result = {};
    status = spg_agent_run(&inputs, &rcfg, &ws, &usage, &loop_result);
    printf("steps=%zu termination=%s journal=%s\n", loop_result.steps_taken,
           spg_agent_loop_termination_to_string(loop_result.termination),
           journal_path);
    if (observation[0] != '\0') {
        printf("observation: %s\n", observation);
    }
    rc = (status == SPG_OK) ? 0 : 1;

done:
    if (journal_open) {
        (void)spg_journal_writer_close(&journal);
    }
    if (model_open) {
        spg_model_adapter_destroy(&model);
    }
    free(policy_path);
    free(scenario_path);
    free(journal_path);
    free(model_path);
    free_file_buffer(&run_text);
    free_file_buffer(&policy_text);
    free_file_buffer(&scenario_text);
    free_file_buffer(&script_text);
    return rc;
}

/* ---- eval suite runner ---- */

/* Find a (name ...) child-list directly under parent; INVALID if absent. */
static uint32_t eval_field(const struct spg_sexpr_node *nodes,
                           const uint32_t parent, const size_t in_n,
                           const char *in, const char *name) {
    for (uint32_t c = spg_sexpr_first_child(nodes, parent);
         c != SPG_SEXPR_INVALID_INDEX; c = nodes[c].next_sibling) {
        if (nodes[c].kind != SPG_SEXPR_NODE_LIST) {
            continue;
        }
        const uint32_t head = spg_sexpr_first_child(nodes, c);
        if (head != SPG_SEXPR_INVALID_INDEX &&
            nodes[head].kind == SPG_SEXPR_NODE_SYMBOL &&
            spg_sexpr_span_eq_cstr(in_n, in, nodes[head].span, name)) {
            return c;
        }
    }
    return SPG_SEXPR_INVALID_INDEX;
}

static bool eval_str(const struct spg_sexpr_node *nodes, const uint32_t parent,
                     const size_t in_n, const char *in, const char *name,
                     char *out, const size_t cap) {
    const uint32_t f = eval_field(nodes, parent, in_n, in, name);
    if (f == SPG_SEXPR_INVALID_INDEX) {
        return false;
    }
    const uint32_t v = spg_sexpr_second_child(nodes, f);
    struct spg_text_span sp;
    if (v == SPG_SEXPR_INVALID_INDEX ||
        !spg_sexpr_string_payload_span(&nodes[v], &sp) || sp.length + 1u > cap) {
        return false;
    }
    memcpy(out, in + sp.offset, sp.length);
    out[sp.length] = '\0';
    return true;
}

static bool eval_u64(const struct spg_sexpr_node *nodes, const uint32_t parent,
                     const size_t in_n, const char *in, const char *name,
                     uint64_t *out) {
    const uint32_t f = eval_field(nodes, parent, in_n, in, name);
    if (f == SPG_SEXPR_INVALID_INDEX) {
        return false;
    }
    const uint32_t v = spg_sexpr_second_child(nodes, f);
    if (v == SPG_SEXPR_INVALID_INDEX || nodes[v].kind != SPG_SEXPR_NODE_SYMBOL) {
        return false;
    }
    return spg_sexpr_parse_uint64_span(in_n, in, nodes[v].span, out) == SPG_OK;
}

static bool eval_flag(const struct spg_sexpr_node *nodes, const uint32_t parent,
                      const size_t in_n, const char *in, const char *name) {
    const uint32_t f = eval_field(nodes, parent, in_n, in, name);
    if (f == SPG_SEXPR_INVALID_INDEX) {
        return false;
    }
    const uint32_t v = spg_sexpr_second_child(nodes, f);
    return v != SPG_SEXPR_INVALID_INDEX &&
           nodes[v].kind == SPG_SEXPR_NODE_SYMBOL &&
           spg_sexpr_span_eq_cstr(in_n, in, nodes[v].span, "true");
}

/* Map an (expect (termination <sym>) ...) symbol to the enum. */
static bool eval_termination(const struct spg_sexpr_node *nodes,
                             const uint32_t expect, const size_t in_n,
                             const char *in,
                             enum spg_agent_loop_termination *out) {
    const uint32_t f = eval_field(nodes, expect, in_n, in, "termination");
    if (f == SPG_SEXPR_INVALID_INDEX) {
        return false;
    }
    const uint32_t v = spg_sexpr_second_child(nodes, f);
    if (v == SPG_SEXPR_INVALID_INDEX || nodes[v].kind != SPG_SEXPR_NODE_SYMBOL) {
        return false;
    }
    static const enum spg_agent_loop_termination all[] = {
        SPG_AGENT_LOOP_FINISHED, SPG_AGENT_LOOP_MAX_STEPS,
        SPG_AGENT_LOOP_REJECTED, SPG_AGENT_LOOP_DENIED,
        SPG_AGENT_LOOP_BUDGET,   SPG_AGENT_LOOP_ERROR};
    for (size_t i = 0u; i < sizeof all / sizeof all[0]; i += 1u) {
        if (spg_sexpr_span_eq_cstr(in_n, in, nodes[v].span,
                                   spg_agent_loop_termination_to_string(all[i]))) {
            *out = all[i];
            return true;
        }
    }
    return false;
}

#define EVAL_SCRIPT_MAX 64u

#define EVAL_MAX_CASES 64u

struct eval_run_report {
    size_t                      total;  /* individual runs (cases x samples)   */
    size_t                      passed; /* individual passing runs             */
    size_t                      ncases; /* distinct cases (index into the rest) */
    char                        names[EVAL_MAX_CASES][64];
    struct spg_eval_case_result results[EVAL_MAX_CASES]; /* last sample/case   */
    size_t                      runs[EVAL_MAX_CASES];        /* samples per case */
    size_t                      case_passed[EVAL_MAX_CASES]; /* k passed of N    */
};

/* Per-invocation knobs. A null pointer (or zeroed struct) reproduces the
 * historical behaviour: one sample per case, no remote endpoint. */
struct eval_run_opts {
    const char *remote_url;   /* nullable: enables (model "remote") cases      */
    const char *remote_model; /* nullable: model name for remote cases         */
    size_t      samples;      /* 0/1 => run each case once                     */
};

/* Load a suite + its shared run/policy/scenario config and run every case with
 * the given mind-palace store (nullable) threaded into each run; fill *report.
 * Returns SPG_OK on a complete run. Prints nothing (the caller reports). */
static enum spg_status eval_run_suite(const char *suite_path,
                                      struct spg_mem_store *store,
                                      const struct eval_run_opts *opts,
                                      struct eval_run_report *report) {
    *report = (struct eval_run_report){};
    struct file_buffer suite_text    = {};
    struct file_buffer run_text      = {};
    struct file_buffer policy_text   = {};
    struct file_buffer scenario_text = {};
    char              *policy_path   = nullptr;
    char              *scenario_path = nullptr;
    char              *model_path    = nullptr;
    enum spg_status    rc            = SPG_E_IO;

    enum spg_status status = read_file(suite_path, &suite_text);
    if (status != SPG_OK) {
        rc = status;
        goto done;
    }
    static struct spg_sexpr_token tok[CLI_TOKEN_CAPACITY];
    static struct spg_sexpr_node  nod[CLI_NODE_CAPACITY];
    size_t                        tn = 0u;
    size_t                        nn = 0u;
    struct spg_sexpr_error        se = {};
    if (spg_sexpr_parse_text(suite_text.n, suite_text.data, CLI_TOKEN_CAPACITY,
                             tok, CLI_NODE_CAPACITY, nod, &tn, &nn, &se) !=
            SPG_OK ||
        nn == 0u || nod[0].kind != SPG_SEXPR_NODE_LIST) {
        rc = SPG_E_FORMAT;
        goto done;
    }
    char config_path[CLI_PATH_MAX];
    if (!eval_str(nod, 0u, suite_text.n, suite_text.data, "config", config_path,
                  sizeof config_path)) {
        rc = SPG_E_FORMAT;
        goto done;
    }

    struct spg_run_config run = {};
    status = load_run_file(config_path, &run_text, &run);
    if (status == SPG_OK) {
        status = span_to_cstr(run_text.n, run_text.data, run.policy_path,
                              &policy_path);
    }
    if (status == SPG_OK) {
        status = span_to_cstr(run_text.n, run_text.data, run.scenario_path,
                              &scenario_path);
    }
    if (status == SPG_OK) {
        status =
            span_to_cstr(run_text.n, run_text.data, run.model_path, &model_path);
    }
    if (status != SPG_OK) {
        rc = status;
        goto done;
    }
    struct spg_policy_config policy = {};
    struct spg_sim_config    sim    = {};
    if (load_policy_file(policy_path, &policy_text, &policy) != SPG_OK ||
        load_scenario_file(scenario_path, &scenario_text, &sim) != SPG_OK) {
        rc = SPG_E_FORMAT;
        goto done;
    }

    static struct spg_context_graph_ref   graph_refs[CLI_CONTEXT_REFS];
    static struct spg_context_memory_ref  memory_refs[CLI_CONTEXT_REFS];
    static struct spg_context_journal_ref journal_refs[CLI_CONTEXT_REFS];
    static char            context[CLI_CONTEXT_BYTES];
    static char            model_output[CLI_MODEL_OUTPUT_BYTES];
    static struct spg_sexpr_token rtok[CLI_TOKEN_CAPACITY];
    static struct spg_sexpr_node  rnod[CLI_NODE_CAPACITY];
    static char            ppay[CLI_PAYLOAD_BYTES];
    static char            spay[CLI_PAYLOAD_BYTES];
    static char            observation[AGENT_OBS_BYTES];
    static char            sh_out[AGENT_SHELL_STDOUT];
    static char            sh_err[AGENT_SHELL_STDERR];
    static char            mem_index[AGENT_OBS_BYTES];
    static struct spg_journal_record_header traj[256];
    const struct spg_agent_run_workspace ws = {
        .context_capacity        = sizeof context,
        .context                 = context,
        .model_output_capacity   = sizeof model_output,
        .model_output            = model_output,
        .graph_ref_capacity      = CLI_CONTEXT_REFS,
        .graph_refs              = graph_refs,
        .memory_ref_capacity     = CLI_CONTEXT_REFS,
        .memory_refs             = memory_refs,
        .journal_ref_capacity    = CLI_CONTEXT_REFS,
        .journal_refs            = journal_refs,
        .token_capacity          = CLI_TOKEN_CAPACITY,
        .tokens                  = rtok,
        .node_capacity           = CLI_NODE_CAPACITY,
        .nodes                   = rnod,
        .policy_payload_capacity = sizeof ppay,
        .policy_payload          = ppay,
        .sim_payload_capacity    = sizeof spay,
        .sim_payload             = spay,
        .observation_capacity    = sizeof observation,
        .observation             = observation,
        .shell_stdout_capacity   = sizeof sh_out,
        .shell_stdout            = sh_out,
        .shell_stderr_capacity   = sizeof sh_err,
        .shell_stderr            = sh_err,
        .trajectory_capacity     = 256u,
        .trajectory              = traj,
        .memory_index_capacity   = sizeof mem_index,
        .memory_index            = mem_index,
    };
    const struct spg_agent_run_inputs inputs = {
        .policy        = &policy,
        .policy_text_n = policy_text.n,
        .policy_text   = policy_text.data,
        .run           = &run,
        .sim           = &sim,
        .store         = store,
    };

    /* Invocation-wide knobs (resolved once, not per case). */
    const struct eval_run_opts  defaults = {};
    const struct eval_run_opts *o = (opts != nullptr) ? opts : &defaults;
    const size_t                samples = (o->samples > 0u) ? o->samples : 1u;
    const char                 *env_api_url = getenv("GEISTSHELL_API_URL");
    const char                 *remote_url =
        (o->remote_url != nullptr && o->remote_url[0] != '\0') ? o->remote_url
        : (env_api_url != nullptr && env_api_url[0] != '\0')   ? env_api_url
                                                               : nullptr;
    const char *api_key = getenv("GEISTSHELL_API_KEY");

    rc = SPG_OK;
    for (uint32_t c = spg_sexpr_first_child(nod, 0u);
         c != SPG_SEXPR_INVALID_INDEX && report->ncases < EVAL_MAX_CASES;
         c = nod[c].next_sibling) {
        const uint32_t head = spg_sexpr_first_child(nod, c);
        if (nod[c].kind != SPG_SEXPR_NODE_LIST ||
            head == SPG_SEXPR_INVALID_INDEX ||
            !spg_sexpr_span_eq_cstr(suite_text.n, suite_text.data,
                                    nod[head].span, "case")) {
            continue;
        }
        const size_t case_idx = report->ncases;
        char        *name     = report->names[case_idx];
        (void)snprintf(name, sizeof report->names[0], "case");
        char script_path[CLI_PATH_MAX];
        char obs[256];
        (void)eval_str(nod, c, suite_text.n, suite_text.data, "name", name,
                       sizeof report->names[0]);
        const bool has_script =
            eval_str(nod, c, suite_text.n, suite_text.data, "script",
                     script_path, sizeof script_path);
        uint64_t max_steps   = 8u;
        uint64_t max_repairs = 0u;
        (void)eval_u64(nod, c, suite_text.n, suite_text.data, "max_steps",
                       &max_steps);
        (void)eval_u64(nod, c, suite_text.n, suite_text.data, "max_repairs",
                       &max_repairs);
        const bool allow_exec =
            eval_flag(nod, c, suite_text.n, suite_text.data, "allow_exec");
        char        gate[64];
        const char *gate_marker =
            eval_str(nod, c, suite_text.n, suite_text.data, "gate_marker", gate,
                     sizeof gate)
                ? gate
                : nullptr;

        struct spg_eval_expect expect = {};
        const uint32_t exp = eval_field(nod, c, suite_text.n, suite_text.data,
                                        "expect");
        if (exp != SPG_SEXPR_INVALID_INDEX) {
            enum spg_agent_loop_termination term;
            if (eval_termination(nod, exp, suite_text.n, suite_text.data,
                                 &term)) {
                expect.check_termination = true;
                expect.termination       = term;
            }
            uint64_t mn = 0u;
            uint64_t mx = 0u;
            if (eval_u64(nod, exp, suite_text.n, suite_text.data, "min_steps",
                         &mn)) {
                expect.min_steps = (size_t)mn;
            }
            if (eval_u64(nod, exp, suite_text.n, suite_text.data, "max_steps",
                         &mx)) {
                expect.max_steps = (size_t)mx;
            }
            if (eval_str(nod, exp, suite_text.n, suite_text.data, "observation",
                         obs, sizeof obs)) {
                expect.observation = obs;
            }
        }

        const struct spg_agent_run_config rcfg = {
            .max_steps         = (size_t)max_steps,
            .max_repairs       = (size_t)max_repairs,
            .execution_enabled = allow_exec,
            .exec_timeout_ms   = 5000u,
            .exec_stdout_cap   = sizeof sh_out,
            .exec_stderr_cap   = sizeof sh_err,
            .context_refs      = CLI_CONTEXT_REFS,
        };
        char       model_kind[16];
        const bool has_model =
            eval_str(nod, c, suite_text.n, suite_text.data, "model", model_kind,
                     sizeof model_kind);
        const bool geist_case  = has_model && strcmp(model_kind, "geist") == 0;
        const bool remote_case = has_model && strcmp(model_kind, "remote") == 0;

        struct spg_eval_case_result last           = {};
        size_t                      passed_in_case = 0u;
        size_t                      runs_in_case   = 0u;

        if (geist_case || remote_case) {
            /* Real-model case (non-deterministic): build the adapter ONCE and
             * reuse it across the N samples — spg_agent_run resets the session
             * each call, and rebuilding a GEIST adapter per sample would reload
             * the GGUF. */
            struct spg_model_adapter        model = {};
            struct spg_model_adapter_config mc    = {
                   .sampling = {.max_seq_len = 4096u,
                                .temperature = 0.0f,
                                .top_p       = 1.0f,
                                .top_k       = 0,
                                .random_seed = run.seed},
            };
            if (geist_case) {
                mc.kind       = SPG_MODEL_ADAPTER_GEIST;
                mc.model_path = model_path;
            } else {
                mc.kind         = SPG_MODEL_ADAPTER_REMOTE;
                mc.endpoint_url = remote_url; /* may be null -> handled below */
                mc.model_name =
                    (o->remote_model != nullptr && o->remote_model[0] != '\0')
                        ? o->remote_model
                        : model_path;
                mc.api_key = api_key;
            }
            const enum spg_status is =
                (remote_case && remote_url == nullptr)
                    ? SPG_E_INVALID_ARG /* (model "remote") but no endpoint */
                    : spg_model_adapter_init(&model, &mc);
            if (is != SPG_OK) {
                if (geist_case) {
                    rc = SPG_E_MODEL; /* a configured GGUF that won't load aborts */
                    goto done;
                }
                /* Remote misconfig / default (non-REMOTE) build: one clean
                 * failing run, no crash, no whole-suite abort. */
                if (is == SPG_E_UNSUPPORTED) {
                    fprintf(stderr, "eval: rebuild with `make REMOTE=1` to run "
                                    "(model \"remote\") cases\n");
                }
                last = (struct spg_eval_case_result){
                    .outcome = SPG_EVAL_FAIL_RUN_ERROR, .status = is};
                report->total += 1u;
                runs_in_case = 1u;
            } else {
                for (size_t s = 0u; s < samples; s += 1u) {
                    struct spg_agent_run_inputs gin = inputs;
                    gin.model                       = &model;
                    struct spg_policy_usage      usage = {};
                    struct spg_agent_loop_result loop  = {};
                    const enum spg_status        rs =
                        spg_agent_run(&gin, &rcfg, &ws, &usage, &loop);
                    last = (struct spg_eval_case_result){
                        .outcome =
                            spg_eval_judge(&expect, &loop, rs, observation),
                        .termination  = loop.termination,
                        .steps_taken  = loop.steps_taken,
                        .repairs_used = loop.repairs_used,
                        .status       = rs,
                    };
                    if (last.outcome == SPG_EVAL_PASS) {
                        passed_in_case += 1u;
                        report->passed += 1u;
                    }
                    report->total += 1u;
                    runs_in_case += 1u;
                }
                spg_model_adapter_destroy(&model);
            }
        } else {
            if (!has_script) {
                rc = SPG_E_FORMAT; /* a scripted case needs a script */
                goto done;
            }
            struct file_buffer script_text = {};
            if (read_file(script_path, &script_text) != SPG_OK) {
                rc = SPG_E_IO;
                goto done;
            }
            static struct spg_fake_response script[EVAL_SCRIPT_MAX];
            const size_t script_n = split_script_lines(
                script_text.data, script_text.n, script, EVAL_SCRIPT_MAX);
            for (size_t s = 0u; s < samples; s += 1u) {
                struct spg_eval_case_result r  = {};
                const enum spg_status       cs = spg_eval_run_case(
                          script, script_n, gate_marker, &inputs, &rcfg, &ws,
                          &expect, &r);
                if (cs != SPG_OK) {
                    free_file_buffer(&script_text);
                    rc = cs;
                    goto done;
                }
                last = r;
                if (r.outcome == SPG_EVAL_PASS) {
                    passed_in_case += 1u;
                    report->passed += 1u;
                }
                report->total += 1u;
                runs_in_case += 1u;
            }
            free_file_buffer(&script_text);
        }

        report->results[case_idx]     = last;
        report->runs[case_idx]        = runs_in_case;
        report->case_passed[case_idx] = passed_in_case;
        report->ncases += 1u;
    }

done:
    free(policy_path);
    free(scenario_path);
    free(model_path);
    free_file_buffer(&suite_text);
    free_file_buffer(&run_text);
    free_file_buffer(&policy_text);
    free_file_buffer(&scenario_text);
    return rc;
}

static void eval_print_report(const char *suite_path,
                              const struct eval_run_report *report) {
    for (size_t i = 0u; i < report->ncases; i += 1u) {
        const struct spg_eval_case_result *r = &report->results[i];
        printf("{\"name\":\"%s\",\"outcome\":\"%s\",\"termination\":\"%s\","
               "\"steps\":%zu,\"repairs\":%zu",
               report->names[i], spg_eval_outcome_to_string(r->outcome),
               spg_agent_loop_termination_to_string(r->termination),
               r->steps_taken, r->repairs_used);
        /* With --samples N>1, aggregate k-of-N; N==1 stays byte-identical. */
        if (report->runs[i] > 1u) {
            printf(",\"runs\":%zu,\"passed\":%zu", report->runs[i],
                   report->case_passed[i]);
        }
        printf("}\n");
    }
    printf("{\"suite\":\"%s\",\"total\":%zu,\"passed\":%zu}\n", suite_path,
           report->total, report->passed);
}

static int eval_command(int argc, char **argv) {
    const char *suite_path   = nullptr;
    const char *remote_url   = nullptr;
    const char *remote_model = nullptr;
    size_t      samples      = 1u;
    for (int i = 2; i < argc; i += 1) {
        if (strcmp(argv[i], "--remote-url") == 0 && i + 1 < argc) {
            remote_url = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--remote-model") == 0 && i + 1 < argc) {
            remote_model = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--samples") == 0 && i + 1 < argc) {
            if (!parse_positive_size(argv[++i], &samples)) {
                fprintf(stderr, "eval: invalid --samples value\n");
                return 2;
            }
            continue;
        }
        if (suite_path == nullptr && argv[i][0] != '-') {
            suite_path = argv[i];
            continue;
        }
        fprintf(stderr, "eval: unexpected argument: %s\n", argv[i]);
        return 2;
    }
    if (suite_path == nullptr) {
        fprintf(stderr,
                "usage: %s eval <suite.spg> [--remote-url <url>] "
                "[--remote-model <name>] [--samples <N>]\n",
                argv[0]);
        return 2;
    }
    static struct eval_run_report report;
    const struct eval_run_opts    opts = {.remote_url   = remote_url,
                                          .remote_model = remote_model,
                                          .samples      = samples};
    const enum spg_status status = eval_run_suite(suite_path, nullptr, &opts,
                                                  &report);
    if (status != SPG_OK) {
        fprintf(stderr, "eval: suite run failed: %s\n",
                spg_status_to_string(status));
        return 1;
    }
    eval_print_report(suite_path, &report);
    return (report.total > 0u && report.passed == report.total) ? 0 : 1;
}

/* Self-improvement: run the suite, distill a lesson for each failing case,
 * persist each tentatively into the mind-palace, re-run, and keep it only if
 * the pass count did not drop (else revert). Emits a JSONL report. */
static int improve_command(int argc, char **argv) {
    const char *suite_path   = nullptr;
    const char *memory_dir   = nullptr;
    const char *remote_url    = nullptr;
    const char *remote_model  = nullptr;
    const char *validate_path = nullptr;
    size_t      samples       = 1u;
    for (int i = 2; i < argc; i += 1) {
        if (strcmp(argv[i], "--memory-dir") == 0 && i + 1 < argc) {
            memory_dir = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--validate") == 0 && i + 1 < argc) {
            validate_path = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--remote-url") == 0 && i + 1 < argc) {
            remote_url = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--remote-model") == 0 && i + 1 < argc) {
            remote_model = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--samples") == 0 && i + 1 < argc) {
            if (!parse_positive_size(argv[++i], &samples)) {
                fprintf(stderr, "improve: invalid --samples value\n");
                return 2;
            }
            continue;
        }
        if (suite_path == nullptr && argv[i][0] != '-') {
            suite_path = argv[i];
            continue;
        }
        fprintf(stderr, "improve: unexpected argument: %s\n", argv[i]);
        return 2;
    }
    if (suite_path == nullptr) {
        fprintf(stderr,
                "usage: %s improve <suite.spg> [--validate <holdout.spg>] "
                "[--memory-dir <d>] [--remote-url <url>] [--remote-model <name>] "
                "[--samples <N>]\n",
                argv[0]);
        return 2;
    }

    struct spg_mem_store store;
    if (spg_mem_store_open(&store, spg_mem_resolve_dir(memory_dir)) != SPG_OK) {
        fprintf(stderr, "improve: cannot open memory dir %s\n",
                spg_mem_resolve_dir(memory_dir));
        return 1;
    }

    const struct eval_run_opts opts = {.remote_url   = remote_url,
                                       .remote_model = remote_model,
                                       .samples      = samples};
    static struct eval_run_report baseline;
    if (eval_run_suite(suite_path, &store, &opts, &baseline) != SPG_OK) {
        fprintf(stderr, "improve: baseline run failed\n");
        return 1;
    }

    /* Distinct candidate lessons from the failing cases. */
    struct spg_lesson candidates[EVAL_MAX_CASES];
    size_t            ncand = 0u;
    for (size_t i = 0u; i < baseline.ncases; i += 1u) {
        struct spg_lesson lesson;
        if (!spg_reflect_case(&baseline.results[i], &lesson)) {
            continue;
        }
        bool seen = false;
        for (size_t j = 0u; j < ncand; j += 1u) {
            if (strcmp(candidates[j].slug, lesson.slug) == 0) {
                seen = true;
                break;
            }
        }
        if (!seen && ncand < EVAL_MAX_CASES) {
            candidates[ncand] = lesson;
            ncand += 1u;
        }
    }

    /* Hold-out gate: candidate lessons are distilled from the *train* suite
     * (above), but the keep/revert decision is measured on the *validation*
     * suite when one is given — so a lesson is kept only if it generalises to
     * cases it was not derived from, not merely because it fit the suite it came
     * from. Without --validate the gate falls back to the train suite (the
     * historical behaviour, and the output is byte-identical). */
    const char *const gate_path =
        validate_path != nullptr ? validate_path : suite_path;
    size_t cur_passed = baseline.passed;
    if (validate_path != nullptr) {
        static struct eval_run_report gate_baseline;
        if (eval_run_suite(validate_path, &store, &opts, &gate_baseline) !=
            SPG_OK) {
            fprintf(stderr, "improve: validation baseline run failed\n");
            return 1;
        }
        cur_passed = gate_baseline.passed;
    }
    const size_t orig_passed = cur_passed;
    size_t       kept        = 0u;
    for (size_t k = 0u; k < ncand; k += 1u) {
        const struct spg_lesson *lesson = &candidates[k];
        (void)spg_mem_save(&store, lesson->slug, lesson->description,
                           lesson->body); /* tentative */
        static struct eval_run_report trial;
        if (eval_run_suite(gate_path, &store, &opts, &trial) != SPG_OK) {
            fprintf(stderr, "improve: trial run failed\n");
            return 1;
        }
        const bool accepted = spg_improve_accept(cur_passed, trial.passed);
        bool       was_kept = false;
        (void)spg_improve_commit(&store, lesson, accepted, &was_kept);
        if (validate_path != nullptr) {
            printf("{\"lesson\":\"%s\",\"accepted\":%s,\"held_out_passed\":%zu,"
                   "\"trial_passed\":%zu}\n",
                   lesson->slug, accepted ? "true" : "false", cur_passed,
                   trial.passed);
        } else {
            printf("{\"lesson\":\"%s\",\"accepted\":%s,\"baseline_passed\":%zu,"
                   "\"trial_passed\":%zu}\n",
                   lesson->slug, accepted ? "true" : "false", cur_passed,
                   trial.passed);
        }
        if (was_kept) {
            cur_passed = trial.passed;
            kept += 1u;
        }
    }
    if (validate_path != nullptr) {
        printf("{\"suite\":\"%s\",\"validate\":\"%s\",\"held_out_baseline\":%zu,"
               "\"held_out_final\":%zu,\"lessons_kept\":%zu}\n",
               suite_path, validate_path, orig_passed, cur_passed, kept);
    } else {
        printf("{\"suite\":\"%s\",\"baseline_passed\":%zu,\"final_passed\":%zu,"
               "\"lessons_kept\":%zu}\n",
               suite_path, orig_passed, cur_passed, kept);
    }
    return 0;
}

/* Build "<journal>.sig" (or copy an explicit path) into out. */
static void default_sig_path(const char *journal, const char *explicit_sig,
                             char *out, const size_t cap) {
    if (explicit_sig != nullptr) {
        (void)snprintf(out, cap, "%s", explicit_sig);
    } else {
        (void)snprintf(out, cap, "%s.sig", journal);
    }
}

static int seal_journal_command(int argc, char **argv) {
    const char *journal  = nullptr;
    const char *key_path = nullptr;
    const char *sig      = nullptr;
    for (int i = 2; i < argc; i += 1) {
        if (strcmp(argv[i], "--key") == 0 && i + 1 < argc) {
            key_path = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--sig") == 0 && i + 1 < argc) {
            sig = argv[++i];
            continue;
        }
        if (journal == nullptr && argv[i][0] != '-') {
            journal = argv[i];
            continue;
        }
        fprintf(stderr, "seal-journal: unexpected argument: %s\n", argv[i]);
        return 2;
    }
    if (journal == nullptr || key_path == nullptr) {
        fprintf(stderr,
                "usage: %s seal-journal <journal> --key <keyfile> [--sig <p>]\n",
                argv[0]);
        return 2;
    }
    struct file_buffer keyb = {};
    if (read_file(key_path, &keyb) != SPG_OK || keyb.n == 0u) {
        fprintf(stderr, "seal-journal: cannot read key %s\n", key_path);
        free_file_buffer(&keyb);
        return 1;
    }
    char sigpath[CLI_PATH_MAX];
    default_sig_path(journal, sig, sigpath, sizeof sigpath);
    const enum spg_status s =
        spg_journal_seal(journal, sigpath, keyb.n, (const uint8_t *)keyb.data);
    free_file_buffer(&keyb);
    if (s != SPG_OK) {
        fprintf(stderr, "seal-journal: %s\n", spg_status_to_string(s));
        return 1;
    }
    printf("sealed=%s\n", sigpath);
    return 0;
}

/* Verify a journal's keyed seal; prints signed=true/false, non-zero on mismatch. */
static int verify_signed_cli(const char *journal, const char *key_path,
                             const char *sig) {
    struct file_buffer keyb = {};
    if (read_file(key_path, &keyb) != SPG_OK || keyb.n == 0u) {
        fprintf(stderr, "verify-journal: cannot read key %s\n", key_path);
        free_file_buffer(&keyb);
        return 1;
    }
    char sigpath[CLI_PATH_MAX];
    default_sig_path(journal, sig, sigpath, sizeof sigpath);
    bool                  ok = false;
    const enum spg_status s  = spg_journal_verify_signed(
        journal, sigpath, keyb.n, (const uint8_t *)keyb.data, &ok);
    free_file_buffer(&keyb);
    if (s != SPG_OK) {
        fprintf(stderr, "verify-journal: signed check failed: %s\n",
                spg_status_to_string(s));
        return 1;
    }
    printf("signed=%s\n", ok ? "true" : "false");
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 2;
    }

    if (strcmp(argv[1], "version") == 0) {
        printf("geistshell %s\n", SPG_VERSION_STRING);
        printf("libgeist %s\n", geist_version_string());
        return 0;
    }

    if (strcmp(argv[1], "exec") == 0) {
        if (argc < 3) {
            fprintf(stderr, "usage: %s exec <command> [args...]\n", argv[0]);
            return 2;
        }
        return spg_exec_command(argc - 2, argv + 2);
    }

    if (strcmp(argv[1], "memory") == 0) {
        return spg_memory_command(argc - 2, argv + 2);
    }

    if (strcmp(argv[1], "tick") == 0) {
        const int rc = tick_command(argc, argv);
        if (rc == 2) {
            print_tick_usage(argv[0]);
        }
        return rc;
    }

    if (strcmp(argv[1], "run") == 0) {
        const int rc = run_command(argc, argv);
        if (rc == 2) {
            print_run_usage(argv[0]);
        }
        return rc;
    }

    if (strcmp(argv[1], "agent") == 0) {
        return agent_command(argc, argv);
    }

    if (strcmp(argv[1], "eval") == 0) {
        return eval_command(argc, argv);
    }

    if (strcmp(argv[1], "improve") == 0) {
        return improve_command(argc, argv);
    }

    if (strcmp(argv[1], "verify-journal") == 0) {
        const char *journal  = nullptr;
        const char *key_path = nullptr;
        const char *sig      = nullptr;
        for (int i = 2; i < argc; i += 1) {
            if (strcmp(argv[i], "--key") == 0 && i + 1 < argc) {
                key_path = argv[++i];
                continue;
            }
            if (strcmp(argv[i], "--sig") == 0 && i + 1 < argc) {
                sig = argv[++i];
                continue;
            }
            if (journal == nullptr && argv[i][0] != '-') {
                journal = argv[i];
                continue;
            }
            journal = nullptr; /* force usage on a bad argument */
            break;
        }
        if (journal == nullptr) {
            print_verify_journal_usage(argv[0]);
            return 2;
        }
        const int chain_rc = verify_journal_command(journal);
        if (chain_rc != 0 || key_path == nullptr) {
            return chain_rc;
        }
        return verify_signed_cli(journal, key_path, sig);
    }

    if (strcmp(argv[1], "seal-journal") == 0) {
        return seal_journal_command(argc, argv);
    }

    if (strcmp(argv[1], "replay") == 0) {
        if (argc != 3) {
            print_replay_usage(argv[0]);
            return 2;
        }
        return replay_command(argv[2]);
    }

    if (strcmp(argv[1], "policy-check") == 0) {
        if (argc != 3) {
            print_policy_check_usage(argv[0]);
            return 2;
        }
        return policy_check_command(argv[2]);
    }

    if (strcmp(argv[1], "sim-validate") == 0) {
        if (argc != 3) {
            print_sim_validate_usage(argv[0]);
            return 2;
        }
        return sim_validate_command(argv[2]);
    }

    fprintf(stderr, "%s: unknown command\n", argv[1]);
    print_usage(argv[0]);
    return 2;
}
