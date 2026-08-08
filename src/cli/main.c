#define _POSIX_C_SOURCE                                                        \
    200809L /* mkstemp/write/unlink: the guard-gate temp suite */
#if defined(__APPLE__)
#    define _DARWIN_C_SOURCE 1
#endif

#include "geistshell/device.h"
#include "geistshell/geistshell.h"

#include "geistshell/agent_loop.h"
#include "geistshell/agent_run.h"
#include "geistshell/diagnose.h"
#include "geistshell/eval.h"
#include "geistshell/exec_command.h"
#include "geistshell/fixture.h"
#include "geistshell/grammar_mask.h"
#include "geistshell/guard_ring.h"
#include "geistshell/improve.h"
#include "geistshell/machine_fixture.h"
#include "geistshell/cmd_menu.h"
#include "geistshell/mem_command.h"
#include "geistshell/mem_store.h"
#include "geistshell/model_profile.h"
#include "geistshell/process_profile.h"

#include <geist.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h> /* isfinite: --temperature validation */
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h> /* stat: the audit dates a run against a lesson's mint */
#include <time.h>
#include <unistd.h> /* mkstemp/write/close/unlink: the guard-gate temp suite */

#define CLI_TOKEN_CAPACITY 1024u
#define CLI_NODE_CAPACITY 1024u
#define CLI_CONTEXT_BYTES 32768u
#define CLI_MODEL_OUTPUT_BYTES 8192u
#define CLI_PAYLOAD_BYTES 8192u
#define CLI_CONTEXT_REFS 64u
#define CLI_JOURNAL_VERIFY_PAYLOAD_BYTES 8192u
/* Backing store for the constrained decoder's capability names: 32 caps of a
 * plausible length with room to spare. Overflow narrows the mask, never
 * corrupts it (see spg_model_capabilities_from_policy). */
#define CLI_CAP_NAMES_BYTES 2048u
#define CLI_REPLAY_PAYLOAD_BYTES CLI_CONTEXT_BYTES
#define CLI_REPLAY_PREVIEW_BYTES 96u

struct file_buffer {
    size_t n;
    char  *data;
};

static void            free_file_buffer(struct file_buffer *buffer);
static enum spg_status read_file(const char *path, struct file_buffer *out);
static enum spg_status load_policy_file(const char               *path,
                                        struct file_buffer       *policy_text,
                                        struct spg_policy_config *policy);
static enum spg_status load_scenario_file(const char            *path,
                                          struct file_buffer    *scenario_text,
                                          struct spg_sim_config *sim);

static void print_usage(const char *argv0) {
    fprintf(
        stderr,
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
        "  sim-validate     validate and summarize a scenario file\n"
        "  device           read/write a machine over Modbus TCP\n",
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
    fprintf(
        stderr,
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
    fprintf(stderr, "usage: %s replay <journal.sgj> [--payloads]\n", argv0);
}

static void print_policy_check_usage(const char *argv0) {
    fprintf(stderr, "usage: %s policy-check <policy.spg>\n", argv0);
}

static void print_sim_validate_usage(const char *argv0) {
    fprintf(stderr, "usage: %s sim-validate <scenario.spg>\n", argv0);
}

static const char *
policy_capability_kind_name(const enum spg_policy_capability_kind kind) {
    switch (kind) {
    case SPG_POLICY_CAP_LOCAL_SHELL:
        return "local_shell";
    case SPG_POLICY_CAP_SSH_AUTH_PROBE:
        return "ssh_auth_probe";
    case SPG_POLICY_CAP_SIMULATOR:
        return "simulator";
    case SPG_POLICY_CAP_MEMORY:
        return "memory";
    case SPG_POLICY_CAP_MACHINE_PROCESS:
        return "machine_process";
    case SPG_POLICY_CAP_DEVICE:
        return "device";
    }
    return "unknown";
}

static const char *
policy_network_default_name(const enum spg_policy_network_default value) {
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
                                const char                *expected) {
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
                               const char           *field_name,
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

static bool replay_payload_field(const size_t  payload_n,
                                 const uint8_t payload[], const char *form_name,
                                 const char           *field_name,
                                 struct spg_text_span *out) {
    if (payload == nullptr || out == nullptr) {
        return false;
    }
    struct spg_sexpr_token tokens[CLI_TOKEN_CAPACITY];
    struct spg_sexpr_node  nodes[CLI_NODE_CAPACITY];
    struct spg_sexpr_error error       = {};
    size_t                 token_count = 0u;
    size_t                 node_count  = 0u;
    const char            *text        = (const char *)payload;
    const enum spg_status  status      = spg_sexpr_parse_text(
        payload_n, text, CLI_TOKEN_CAPACITY, tokens, CLI_NODE_CAPACITY, nodes,
        &token_count, &node_count, &error);
    if (status != SPG_OK) {
        return false;
    }
    (void)token_count;
    return replay_field_value(payload_n, text, nodes, node_count, form_name,
                              field_name, out);
}

static bool payload_span_valid(const size_t               payload_n,
                               const struct spg_text_span span) {
    return span.offset <= payload_n && span.length <= payload_n - span.offset;
}

static bool payload_span_is_uint(const size_t               payload_n,
                                 const uint8_t              payload[],
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

static bool payload_span_is_bool(const size_t               payload_n,
                                 const uint8_t              payload[],
                                 const struct spg_text_span span) {
    return replay_span_eq_cstr(payload_n, (const char *)payload, span,
                               "true") ||
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

static void print_json_span_string(const size_t               payload_n,
                                   const uint8_t              payload[],
                                   const struct spg_text_span span) {
    if (payload == nullptr || !payload_span_valid(payload_n, span)) {
        printf("null");
        return;
    }
    print_json_bytes(span.length, payload + span.offset);
}

static void print_json_span_value(const size_t               payload_n,
                                  const uint8_t              payload[],
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

static void print_json_preview(const size_t  payload_n,
                               const uint8_t payload[]) {
    const size_t n = payload_n < CLI_REPLAY_PREVIEW_BYTES
                         ? payload_n
                         : CLI_REPLAY_PREVIEW_BYTES;
    print_json_bytes(n, payload);
}

static int verify_journal_command(const char *path) {
    if (path == nullptr) {
        return 2;
    }

    struct spg_journal_reader reader = {};
    enum spg_status           status = spg_journal_reader_open(&reader, path);
    if (status != SPG_OK) {
        fprintf(stderr, "verify-journal: open failed: %s\n",
                spg_status_to_string(status));
        return 1;
    }

    uint8_t                   payload[CLI_JOURNAL_VERIFY_PAYLOAD_BYTES];
    uint64_t                  counts[SPG_JOURNAL_EVENT_ERROR + 1u] = {};
    uint64_t                  total                                = 0u;
    uint64_t                  truncated_payloads                   = 0u;
    uint64_t                  status_failures                      = 0u;
    uint64_t                  payload_bytes                        = 0u;
    uint64_t                  last_sequence                        = 0u;
    struct spg_journal_record record                               = {};

    for (;;) {
        status =
            spg_journal_reader_next(&reader, sizeof payload, payload, &record);
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
    printf("truncated_payloads=%llu\n", (unsigned long long)truncated_payloads);
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
    print_json_bytes(
        strlen(journal_event_kind_name(record->header.event_kind)),
        (const uint8_t *)journal_event_kind_name(record->header.event_kind));
    printf(",\"status\":");
    print_json_bytes(
        strlen(spg_status_to_string((enum spg_status)record->header.status)),
        (const uint8_t *)spg_status_to_string(
            (enum spg_status)record->header.status));
    printf(",\"payload_bytes\":%llu",
           (unsigned long long)record->header.payload_bytes);
    printf(",\"truncated\":%s", truncated ? "true" : "false");
}

static void print_replay_field_json(const size_t  payload_n,
                                    const uint8_t payload[],
                                    const char   *form_name,
                                    const char   *field_name) {
    struct spg_text_span value = {};
    if (!replay_payload_field(payload_n, payload, form_name, field_name,
                              &value)) {
        return;
    }
    printf(",\"%s\":", field_name);
    print_json_span_value(payload_n, payload, value);
}

static void print_replay_policy_json(const size_t  payload_n,
                                     const uint8_t payload[]) {
    const char *fields[] = {"decision", "deny_reason",  "action_kind",
                            "cost",     "uses_network", "confidence_bp"};
    for (size_t i = 0u; i < sizeof fields / sizeof fields[0]; i += 1u) {
        print_replay_field_json(payload_n, payload, "policy_decision",
                                fields[i]);
    }
}

static void print_replay_sim_json(const size_t  payload_n,
                                  const uint8_t payload[]) {
    const char *fields[] = {"action", "selected_index", "mutated",
                            "risk_before", "risk_after"};
    for (size_t i = 0u; i < sizeof fields / sizeof fields[0]; i += 1u) {
        print_replay_field_json(payload_n, payload, "sim_result", fields[i]);
    }
}

/* #56: the journal is the audit surface, but replay printed headers only — the
 * recorded model input, and with it the rendered context, was unreadable.
 * --payloads dumps each record's bytes after its header line, fenced so the
 * JSONL stays parseable by anyone who ignores the fence. */
static int replay_command(const char *path, const bool with_payloads) {
    if (path == nullptr) {
        return 2;
    }

    struct spg_journal_reader reader = {};
    enum spg_status           status = spg_journal_reader_open(&reader, path);
    if (status != SPG_OK) {
        fprintf(stderr, "replay: open failed: %s\n",
                spg_status_to_string(status));
        return 1;
    }

    uint8_t                   payload[CLI_REPLAY_PAYLOAD_BYTES];
    struct spg_journal_record record = {};

    for (;;) {
        status =
            spg_journal_reader_next(&reader, sizeof payload, payload, &record);
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
        if (with_payloads && available_payload > 0u) {
            printf("--- payload %llu ---\n",
                   (unsigned long long)record.header.sequence);
            (void)fwrite(payload, 1u, available_payload, stdout);
            printf("\n--- end %llu ---\n",
                   (unsigned long long)record.header.sequence);
        }
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
    struct file_buffer       policy_text = {};
    struct spg_policy_config policy      = {};
    const enum spg_status    status =
        load_policy_file(path, &policy_text, &policy);
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
    errno                          = 0;
    char                    *end   = nullptr;
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

/* Sampling temperature: finite and >= 0 (0 = greedy). Validated here rather
 * than letting a typo reach the adapter as a NaN, where it surfaces as an
 * opaque model-init failure. */
static bool parse_temperature(const char *text, float *out) {
    if (text == nullptr || out == nullptr || text[0] == '\0') {
        return false;
    }
    char        *end = nullptr;
    const double v   = strtod(text, &end);
    if (end == text || *end != '\0' || !isfinite(v) || v < 0.0) {
        return false;
    }
    *out = (float)v;
    return true;
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

static uint64_t
latest_result_sequence(const struct spg_orchestrator_result *result,
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

/* Set by SIGINT/SIGTERM and read by the cleanup path. A handler may only touch
 * a volatile sig_atomic_t and call async-signal-safe functions; resuming
 * processes from inside it would mean journalling from a signal handler, so the
 * handler does the one safe thing and the normal path does the work. */
static volatile sig_atomic_t interrupt_requested = 0;

static void note_interrupt(int signum) {
    (void)signum;
    interrupt_requested = 1;
}

static void install_interrupt_handler(void) {
    struct sigaction sa = {};
    sa.sa_handler       = note_interrupt;
    (void)sigemptyset(&sa.sa_mask);
    /* No SA_RESTART on purpose: a blocking read should return EINTR so the run
     * unwinds to the cleanup path instead of sitting there while a process
     * stays stopped. */
    sa.sa_flags = 0;
    (void)sigaction(SIGINT, &sa, nullptr);
    (void)sigaction(SIGTERM, &sa, nullptr);
}

/* One machine run per journal, enforced by an advisory lock on a sibling file.
 *
 * Recovery reads the journal and resumes whatever looks stranded — with two
 * runs sharing a journal it would resume the other run's pauses mid-flight.
 * The lock also prevents something that was already broken and simply had not
 * been hit: two writers appending to one hash-chained journal interleave their
 * records and destroy the chain.
 *
 * Advisory and non-blocking: a second run is told to use its own journal
 * rather than made to wait for a run that may take hours. Returns -1 when the
 * lock is held elsewhere. */
static int lock_machine_journal(const char *journal_path) {
    char path[4096];
    if (snprintf(path, sizeof path, "%s.lock", journal_path) < 0) {
        return -1;
    }
    const int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        return -1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        (void)close(fd);
        return -1;
    }
    return fd;
}

/* Wall clock for measurement only — never for anything the journal records.
 * A benchmark has to know how long a thing took; the runtime still must not. */
static uint64_t bench_now_ms(void) {
    struct timespec ts = {};
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/* Peak resident set of this process, in kilobytes. Linux reports ru_maxrss in
 * KB and macOS in bytes — a difference that would otherwise show up as a
 * thousandfold jump between hosts in the results table. */
static uint64_t bench_peak_rss_kb(void) {
    struct rusage usage = {};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0u;
    }
#if defined(__APPLE__)
    return (uint64_t)usage.ru_maxrss / 1024u;
#else
    return (uint64_t)usage.ru_maxrss;
#endif
}

static void update_run_usage(struct spg_policy_usage              *usage,
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

static void print_run_tick_summary(const size_t tick_index,
                                   const struct spg_orchestrator_result *result,
                                   const struct spg_policy_usage *usage) {
    printf("tick=%zu", tick_index);
    printf(" stage=%s", spg_orchestrator_stage_to_string(result->stage));
    printf(" recommendation=%s", spg_orchestrator_recommendation_valid(result)
                                     ? "valid"
                                     : "rejected");
    if (!spg_orchestrator_recommendation_valid(result)) {
        printf(" reject_reason=%s", spg_recommendation_reject_reason_to_string(
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
    printf(" consumed.tokens=%llu", (unsigned long long)usage->consumed.tokens);
    printf(" consumed.sim_actions=%llu",
           (unsigned long long)usage->consumed.sim_actions);
    printf("\n");
}

static bool file_span_valid(const size_t               text_n,
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
                                       const char                 text[],
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

static enum spg_status write_sim_state_file(const char  *path,
                                            const size_t source_n,
                                            const char   source[],
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

/* Read and write a real machine through its channel programs.
 *
 * A CLI command rather than an agent action, and in that order on purpose: the
 * fan of phase 15 existed as an action in the policy, the grammar mask and this
 * very switch, and nothing ever executed it. An actuator gets a working
 * executor first and a model-reachable action only once it demonstrably moves
 * something.
 *
 *   geistshell device --config plant.spg \
 *       [--channel '(channel (name "x") (program "p") (range 0 100))'] \
 *       read temp | write heater 60 | list
 */
static void print_device_usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s device [--config <device.spg>] "
            "[--channel '(channel ...)'] ... <read NAME|write NAME "
            "VALUE|list>\n\n"
            "A channel runs its (program ...): no argument to read one "
            "integer from\nstdout, the value as the single argument to "
            "write. The range is a refusal\nbound, not a clamp: an "
            "out-of-range write is rejected and nothing runs.\n",
            argv0);
}

static int device_command(const int argc, char **argv) {
    struct spg_device dev = {};
    spg_device_init(&dev);

    int i = 2;
    for (; i < argc; i += 1) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            struct file_buffer text = {};
            if (read_file(argv[i + 1], &text) != SPG_OK) {
                fprintf(stderr, "device: cannot read config %s\n",
                        argv[i + 1]);
                return 2;
            }
            size_t                bad    = 0u;
            const enum spg_status status =
                spg_device_load(text.n, text.data, &dev, &bad);
            free_file_buffer(&text);
            if (status != SPG_OK) {
                fprintf(stderr, "device: invalid config %s: %s (channel %zu)\n",
                        argv[i + 1], spg_status_to_string(status), bad);
                return 2;
            }
            i += 1;
        } else if (strcmp(argv[i], "--channel") == 0 && i + 1 < argc) {
            struct spg_device_channel channel = {};
            enum spg_status           status  = spg_device_parse_channel(
                strlen(argv[i + 1]), argv[i + 1], &channel);
            if (status == SPG_OK) {
                status = spg_device_add_channel(&dev, &channel);
            }
            if (status != SPG_OK) {
                fprintf(stderr, "device: bad channel '%s': %s\n", argv[i + 1],
                        spg_status_to_string(status));
                return 2;
            }
            i += 1;
        } else {
            break;
        }
    }

    if (i >= argc) {
        print_device_usage(argv[0]);
        return 2;
    }

    if (strcmp(argv[i], "list") == 0) {
        for (size_t c = 0u; c < dev.n_channels; c += 1u) {
            const struct spg_device_channel *ch = &dev.channels[c];
            printf("(channel (name \"%s\") (program \"%s\") "
                   "(range %lld %lld)",
                   ch->name, ch->program, (long long)ch->min,
                   (long long)ch->max);
            if (ch->writable) {
                printf(" (safe %lld)", (long long)ch->safe);
            }
            printf(")\n");
        }
        return 0;
    }

    const bool is_read  = strcmp(argv[i], "read") == 0;
    const bool is_write = strcmp(argv[i], "write") == 0;
    if ((!is_read && !is_write) || i + 1 >= argc ||
        (is_write && i + 2 >= argc)) {
        print_device_usage(argv[0]);
        return 2;
    }
    const char *name  = argv[i + 1];
    int64_t     value = 0;
    if (is_write) {
        char *end = nullptr;
        value     = (int64_t)strtoll(argv[i + 2], &end, 10);
        if (end == argv[i + 2] || *end != '\0') {
            fprintf(stderr, "device: bad value: %s\n", argv[i + 2]);
            return 2;
        }
    }

    /* A write is refused before the socket is opened, so a rejected command
     * costs no connection and reaches no machine. */
    if (is_write) {
        const struct spg_device_channel *ch = spg_device_find(&dev, name);
        if (ch == nullptr) {
            fprintf(stderr, "device: no channel '%s'\n", name);
            return 1;
        }
        if (!ch->writable || value < ch->min || value > ch->max) {
            fprintf(
                stderr, "device: refused %s=%lld (channel is %s, %lld..%lld)\n",
                name, (long long)value, ch->writable ? "writable" : "read-only",
                (long long)ch->min, (long long)ch->max);
            return 1;
        }
    }

    int             exit_code = 0;
    enum spg_status status    = SPG_OK;
    if (is_read) {
        int64_t reading = 0;
        status          = spg_device_read(&dev, name, &reading);
        if (status == SPG_OK) {
            printf("(reading (channel \"%s\") (value %lld))\n", name,
                   (long long)reading);
        } else {
            fprintf(stderr, "device: read %s failed: %s\n", name,
                    spg_status_to_string(status));
            exit_code = 1;
        }
    } else {
        status = spg_device_write(&dev, name, value);
        if (status == SPG_OK) {
            printf("(wrote (channel \"%s\") (value %lld))\n", name,
                   (long long)value);
        } else {
            fprintf(stderr, "device: write %s=%lld failed: %s\n", name,
                    (long long)value, spg_status_to_string(status));
            exit_code = 1;
        }
    }
    return exit_code;
}

static int sim_validate_command(const char *path) {
    if (path == nullptr) {
        return 2;
    }
    struct file_buffer    scenario_text = {};
    struct spg_sim_config sim           = {};
    enum spg_status status = load_scenario_file(path, &scenario_text, &sim);
    if (status != SPG_OK) {
        fprintf(stderr, "sim-validate: load failed: %s\n",
                spg_status_to_string(status));
        free_file_buffer(&scenario_text);
        return 1;
    }
    struct spg_risk_score risk = {};
    status                     = spg_risk_evaluate(&sim, &risk);
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

/* free + null the caller's pointer. Was geist's safe_free, reached through a
 * PRIVATE engine header (deps/geist/src/base/heap.h) that moved in v0.3.1; the
 * CLI already mixed it with plain free(), so this is the same behaviour without
 * the cross-repo coupling. */
static void free_ptr(void **ptr) {
    if (ptr != nullptr && *ptr != nullptr) {
        free(*ptr);
        *ptr = nullptr;
    }
}

static void free_file_buffer(struct file_buffer *buffer) {
    if (buffer == nullptr) {
        return;
    }
    free_ptr((void **)&buffer->data);
    buffer->n = 0u;
}

static enum spg_status read_file(const char *path, struct file_buffer *out) {
    if (path == nullptr || out == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out       = (struct file_buffer){};
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
    const size_t n    = (size_t)end;
    char        *data = malloc(n + 1u);
    if (data == nullptr) {
        (void)fclose(file);
        return SPG_E_OOM;
    }
    if (n > 0u && fread(data, 1u, n, file) != n) {
        free_ptr((void **)&data);
        (void)fclose(file);
        return SPG_E_IO;
    }
    if (fclose(file) != 0) {
        free_ptr((void **)&data);
        return SPG_E_IO;
    }
    data[n]   = '\0';
    out->n    = n;
    out->data = data;
    return SPG_OK;
}

static enum spg_status span_to_cstr(const size_t input_n, const char input[],
                                    const struct spg_text_span span,
                                    char                     **out) {
    if (input == nullptr || out == nullptr || span.offset > input_n ||
        span.length > input_n - span.offset) {
        return SPG_E_INVALID_ARG;
    }
    *out       = nullptr;
    char *text = malloc(span.length + 1u);
    if (text == nullptr) {
        return SPG_E_OOM;
    }
    memcpy(text, input + span.offset, span.length);
    text[span.length] = '\0';
    *out              = text;
    return SPG_OK;
}

static enum spg_status load_run_file(const char            *path,
                                     struct file_buffer    *run_text,
                                     struct spg_run_config *run) {
    enum spg_status status = read_file(path, run_text);
    if (status != SPG_OK) {
        return status;
    }
    struct spg_sexpr_token      tokens[CLI_TOKEN_CAPACITY];
    struct spg_sexpr_node       nodes[CLI_NODE_CAPACITY];
    struct spg_run_config_error error = {};
    status =
        spg_run_config_load(run_text->n, run_text->data, CLI_TOKEN_CAPACITY,
                            tokens, CLI_NODE_CAPACITY, nodes, run, &error);
    return status;
}

static enum spg_status load_policy_file(const char               *path,
                                        struct file_buffer       *policy_text,
                                        struct spg_policy_config *policy) {
    enum spg_status status = read_file(path, policy_text);
    if (status != SPG_OK) {
        return status;
    }
    struct spg_sexpr_token         tokens[CLI_TOKEN_CAPACITY];
    struct spg_sexpr_node          nodes[CLI_NODE_CAPACITY];
    struct spg_policy_config_error error = {};
    status = spg_policy_config_load(policy_text->n, policy_text->data,
                                    CLI_TOKEN_CAPACITY, tokens,
                                    CLI_NODE_CAPACITY, nodes, policy, &error);
    return status;
}

static enum spg_status load_scenario_file(const char            *path,
                                          struct file_buffer    *scenario_text,
                                          struct spg_sim_config *sim) {
    enum spg_status status = read_file(path, scenario_text);
    if (status != SPG_OK) {
        return status;
    }
    struct spg_sexpr_token      tokens[CLI_TOKEN_CAPACITY];
    struct spg_sexpr_node       nodes[CLI_NODE_CAPACITY];
    struct spg_sim_config_error error = {};
    status = spg_sim_config_load(scenario_text->n, scenario_text->data,
                                 CLI_TOKEN_CAPACITY, tokens, CLI_NODE_CAPACITY,
                                 nodes, sim, &error);
    return status;
}

static int run_tick_fake(const char *run_path, const char *fake_output) {
    if (run_path == nullptr || fake_output == nullptr) {
        return 2;
    }

    int                       rc            = 1;
    struct file_buffer        run_text      = {};
    struct file_buffer        policy_text   = {};
    struct file_buffer        scenario_text = {};
    char                     *policy_path   = nullptr;
    char                     *scenario_path = nullptr;
    char                     *journal_path  = nullptr;
    struct spg_journal_writer journal       = {};
    bool                      journal_open  = false;
    struct spg_model_adapter  model         = {};
    bool                      model_open    = false;

    struct spg_run_config run    = {};
    enum spg_status       status = load_run_file(run_path, &run_text, &run);
    if (status != SPG_OK) {
        fprintf(stderr, "tick: load run failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }
    status =
        span_to_cstr(run_text.n, run_text.data, run.policy_path, &policy_path);
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

    struct spg_graph  graph  = {};
    struct spg_memory memory = {};
    spg_graph_init(&graph);
    spg_memory_init(&memory);

    struct spg_context_graph_ref   graph_refs[CLI_CONTEXT_REFS];
    struct spg_context_memory_ref  memory_refs[CLI_CONTEXT_REFS];
    struct spg_context_journal_ref journal_refs[CLI_CONTEXT_REFS];
    char                           context[CLI_CONTEXT_BYTES];
    char                           model_output[CLI_MODEL_OUTPUT_BYTES];
    struct spg_sexpr_token         rec_tokens[CLI_TOKEN_CAPACITY];
    struct spg_sexpr_node          rec_nodes[CLI_NODE_CAPACITY];
    char                           policy_payload[CLI_PAYLOAD_BYTES];
    char                           sim_payload[CLI_PAYLOAD_BYTES];

    const struct spg_orchestrator_workspace workspace = {
        .actor =
            {
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

    struct spg_policy_usage       usage = {};
    /* #56: the same command menu the agent path renders. Deliberately NOT
     * conditional on the subcommand — a context that differs between `run` and
     * `agent` is a silent inconsistency, and this repo has no CI to catch the
     * day it starts to matter. It does move the baseline journal hash; see
     * test/test_cli_baseline.sh. */
    static char            run_tools[4096];
    struct spg_host_info   run_host = {};
    (void)spg_host_probe(&run_host);
    (void)spg_cmd_menu_render(run_host.os, sizeof run_tools, run_tools);

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
        .tools         = run_tools,
    };
    const struct spg_orchestrator_config config = {
        .actor_id            = 1u,
        .timestamp_ns        = 1u,
        .context_limits      = {.graph_nodes    = CLI_CONTEXT_REFS,
                                .memory_facts   = CLI_CONTEXT_REFS,
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
    printf("recommendation=%s\n", spg_orchestrator_recommendation_valid(&result)
                                      ? "valid"
                                      : "rejected");
    if (!spg_orchestrator_recommendation_valid(&result)) {
        printf("reject_reason=%s\n", spg_recommendation_reject_reason_to_string(
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
    free_ptr((void **)&policy_path);
    free_ptr((void **)&scenario_path);
    free_ptr((void **)&journal_path);
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

    int                       rc            = 1;
    struct file_buffer        run_text      = {};
    struct file_buffer        policy_text   = {};
    struct file_buffer        scenario_text = {};
    char                     *policy_path   = nullptr;
    char                     *scenario_path = nullptr;
    char                     *journal_path  = nullptr;
    char                     *model_path    = nullptr;
    struct spg_journal_writer journal       = {};
    bool                      journal_open  = false;
    struct spg_model_adapter  model         = {};
    bool                      model_open    = false;

    struct spg_run_config run    = {};
    enum spg_status       status = load_run_file(run_path, &run_text, &run);
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

    status =
        span_to_cstr(run_text.n, run_text.data, run.policy_path, &policy_path);
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
    status =
        span_to_cstr(run_text.n, run_text.data, run.model_path, &model_path);
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
        (remote_url != nullptr && remote_url[0] != '\0')     ? remote_url
        : (env_api_url != nullptr && env_api_url[0] != '\0') ? env_api_url
                                                             : nullptr;
    const bool use_remote = !use_fake && url != nullptr;
    /* For REMOTE the (model "...") value is a model name, not a file path; an
     * explicit --remote-model overrides it. The key is env-only. */
    const char *remote_name =
        (remote_model != nullptr && remote_model[0] != '\0') ? remote_model
                                                             : model_path;
    const enum spg_model_adapter_kind kind = use_fake ? SPG_MODEL_ADAPTER_FAKE
                                             : use_remote
                                                 ? SPG_MODEL_ADAPTER_REMOTE
                                                 : SPG_MODEL_ADAPTER_GEIST;
    const struct spg_model_adapter_config model_config = {
        .kind       = kind,
        .model_path = (kind == SPG_MODEL_ADAPTER_GEIST) ? model_path : nullptr,
        .endpoint_url    = use_remote ? url : nullptr,
        .model_name      = use_remote ? remote_name : nullptr,
        .api_key         = use_remote ? getenv("GEISTSHELL_API_KEY") : nullptr,
        .sampling        = {.max_seq_len = 4096u,
                            .temperature = 0.0f,
                            .top_p       = 1.0f,
                            .top_k       = 0,
                            .random_seed = run.seed},
        .fake_response_n = use_fake ? strlen(fake_output) : 0u,
        .fake_response   = fake_output,
    };
    status = spg_model_adapter_init(&model, &model_config);
    if (status != SPG_OK) {
        fprintf(stderr, "run: model init failed: %s\n",
                spg_status_to_string(status));
        if (use_remote && status == SPG_E_UNSUPPORTED) {
            fprintf(
                stderr,
                "run: rebuild with `make REMOTE=1` to enable --remote-url\n");
        }
        goto done;
    }
    model_open = true;

    struct spg_graph  graph  = {};
    struct spg_memory memory = {};
    spg_graph_init(&graph);
    spg_memory_init(&memory);

    struct spg_context_graph_ref   graph_refs[CLI_CONTEXT_REFS];
    struct spg_context_memory_ref  memory_refs[CLI_CONTEXT_REFS];
    struct spg_context_journal_ref journal_refs[CLI_CONTEXT_REFS];
    char                           context[CLI_CONTEXT_BYTES];
    char                           model_output[CLI_MODEL_OUTPUT_BYTES];
    struct spg_sexpr_token         rec_tokens[CLI_TOKEN_CAPACITY];
    struct spg_sexpr_node          rec_nodes[CLI_NODE_CAPACITY];
    char                           policy_payload[CLI_PAYLOAD_BYTES];
    char                           sim_payload[CLI_PAYLOAD_BYTES];
    char                           mem_recall_buf[8192] = {0};

    const struct spg_orchestrator_workspace workspace = {
        .actor =
            {
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
        .observation_capacity          = sizeof mem_recall_buf,
        .observation_buf               = mem_recall_buf,
    };

    struct spg_policy_usage       usage               = {};
    char                          mem_index_buf[4096] = {0};
    /* #56: same menu as run_tick_fake and the agent path — one context, not
     * one per subcommand. */
    static char          loop_tools[4096];
    struct spg_host_info loop_host = {};
    (void)spg_host_probe(&loop_host);
    (void)spg_cmd_menu_render(loop_host.os, sizeof loop_tools, loop_tools);

    struct spg_orchestrator_state state               = {
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
        .observation   = have_store ? mem_recall_buf : nullptr,
        .tools         = loop_tools,
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
            .context_limits      = {.graph_nodes    = CLI_CONTEXT_REFS,
                                    .memory_facts   = CLI_CONTEXT_REFS,
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
    status                           = spg_risk_evaluate(&sim, &final_risk);
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
    free_ptr((void **)&policy_path);
    free_ptr((void **)&scenario_path);
    free_ptr((void **)&journal_path);
    free_ptr((void **)&model_path);
    free_file_buffer(&run_text);
    free_file_buffer(&policy_text);
    free_file_buffer(&scenario_text);
    return rc;
}

static int tick_command(int argc, char **argv) {
    const char *run_path    = nullptr;
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
    const char *run_path       = nullptr;
    const char *fake_output    = nullptr;
    const char *sim_state_path = nullptr;
    const char *run_state_path = nullptr;
    const char *memory_dir     = getenv("GEISTSHELL_MEMORY_DIR");
    const char *remote_url     = nullptr;
    const char *remote_model   = nullptr;
    size_t      ticks          = 3u;
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

#define AGENT_MAX_SCRIPT 64u
#define AGENT_SHELL_STDOUT 4096u
#define AGENT_SHELL_STDERR 1024u
#define AGENT_OBS_BYTES 8192u
#define CLI_PATH_MAX 4096u

/* Split a script buffer into one fake reply per non-blank line (pointers into
 * data; no copy, no NUL needed). Returns the number of replies. */
static size_t split_script_lines(char *data, const size_t n,
                                 struct spg_fake_response out[static 1],
                                 const size_t             cap) {
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
    const char *run_path       = nullptr;
    const char *script_path    = nullptr;
    const char *exemplars_path = nullptr;
    const char *memory_dir     = getenv("GEISTSHELL_MEMORY_DIR");
    const char *directive_slug = nullptr;
    size_t      max_steps      = 8u;
    size_t      max_repairs    = 2u;
    bool        allow_exec     = false;
    bool        constrained    = false;
    float    sample_temp   = 0.0f; /* >0 lets the free slots vary (best-of-N) */
    bool     has_seed      = false;
    uint64_t seed_override = 0u; /* per-attempt seed for best-of-N sampling */
    size_t   best_of       = 1u; /* #2: verifier-guided best-of-N attempts */
    /* Declared up here because every early `goto done` must find it defined —
     * the cleanup path releases it. */
    int machine_lock = -1;
    /* Milliseconds to let the machine settle before re-observing. Default 0
     * keeps a scripted run synchronous; a live experiment needs enough for the
     * counters to reflect its own action. */
    uint64_t    settle_ms    = 0u;
    const char *profile_path = nullptr; /* (process-profile ...) file */
    const char *model_profile_path = nullptr; /* (model_profile ...) file */
    /* Attached machine (device_write). Declared per run: a channel table is
     * the operator saying what this agent may move, which is not something a
     * model or a runtime-discovered config should be able to widen. */
    struct spg_device device = {};
    /* In STEPS, not milliseconds: this loop's injected clock is `step + 1`.
     * Two means the machine may miss one decision's worth of contact before
     * it is driven to its safe state. */
    uint64_t device_watchdog_steps = 2u;
    /* One tick's readings. Lives here rather than inside spg_device so the
     * port stays free of loop state and the block can be parsed back out of
     * an eval fixture. */
    struct spg_device_state device_state = {};
    spg_device_init(&device);
    /* #56: the commands the model is TOLD about. Untrusted model input — it
     * never widens or narrows what the executor permits (cmd_menu.h). */
    const char *menu_path    = nullptr;
    bool        command_mask = false;
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
        if (strcmp(argv[i], "--exemplars") == 0 && i + 1 < argc) {
            exemplars_path = argv[i + 1];
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
        if (strcmp(argv[i], "--constrained") == 0) {
            /* #34: force the real model's output to begin with the
             * recommendation opening, then decode freely. */
            constrained = true;
            continue;
        }
        if (strcmp(argv[i], "--directive-slug") == 0 && i + 1 < argc) {
            /* render this stored lesson's directive every step (strong channel)
             */
            directive_slug = argv[i + 1];
            i += 1;
            continue;
        }
        if (strcmp(argv[i], "--temperature") == 0 && i + 1 < argc) {
            if (!parse_temperature(argv[i + 1], &sample_temp)) {
                fprintf(stderr, "agent: invalid --temperature value\n");
                return 2;
            }
            i += 1;
            continue;
        }
        if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed_override = (uint64_t)strtoull(argv[i + 1], nullptr, 10);
            has_seed      = true;
            i += 1;
            continue;
        }
        if (strcmp(argv[i], "--machine-settle-ms") == 0 && i + 1 < argc) {
            settle_ms = (uint64_t)strtoull(argv[i + 1], nullptr, 10);
            i += 1;
            continue;
        }
        if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
            struct file_buffer device_text = {};
            if (read_file(argv[i + 1], &device_text) != SPG_OK) {
                fprintf(stderr, "agent: cannot read device config %s\n",
                        argv[i + 1]);
                return 2;
            }
            size_t                bad     = 0u;
            const enum spg_status dstatus = spg_device_load(
                device_text.n, device_text.data, &device, &bad);
            free_file_buffer(&device_text);
            if (dstatus != SPG_OK) {
                fprintf(stderr, "agent: invalid device config %s: %s"
                                " (channel %zu)\n",
                        argv[i + 1], spg_status_to_string(dstatus), bad);
                return 2;
            }
            i += 1;
            continue;
        }
        if (strcmp(argv[i], "--device-watchdog-steps") == 0 && i + 1 < argc) {
            char      *end = nullptr;
            const long ms  = strtol(argv[i + 1], &end, 10);
            if (end == argv[i + 1] || *end != '\0' || ms < 0) {
                fprintf(stderr, "agent: bad --device-watchdog-steps: %s\n",
                        argv[i + 1]);
                return 2;
            }
            device_watchdog_steps = (uint64_t)ms;
            i += 1;
            continue;
        }
        if (strcmp(argv[i], "--device-channel") == 0 && i + 1 < argc) {
            struct spg_device_channel channel = {};
            enum spg_status           st      = spg_device_parse_channel(
                strlen(argv[i + 1]), argv[i + 1], &channel);
            if (st == SPG_OK) {
                st = spg_device_add_channel(&device, &channel);
            }
            if (st != SPG_OK) {
                fprintf(stderr, "agent: bad --device-channel '%s': %s\n",
                        argv[i + 1], spg_status_to_string(st));
                return 2;
            }
            i += 1;
            continue;
        }
        if (strcmp(argv[i], "--command-menu") == 0 && i + 1 < argc) {
            menu_path = argv[i + 1];
            i += 1;
            continue;
        }
        if (strcmp(argv[i], "--command-mask") == 0) {
            command_mask = true;
            continue;
        }
        if (strcmp(argv[i], "--process-profile") == 0 && i + 1 < argc) {
            profile_path = argv[i + 1];
            i += 1;
            continue;
        }
        if (strcmp(argv[i], "--model-profile") == 0 && i + 1 < argc) {
            model_profile_path = argv[i + 1];
            i += 1;
            continue;
        }
        if (strcmp(argv[i], "--best-of") == 0 && i + 1 < argc) {
            best_of = (size_t)strtoull(argv[i + 1], nullptr, 10);
            if (best_of == 0u) {
                best_of = 1u;
            }
            i += 1;
            continue;
        }
        fprintf(stderr, "agent: unknown or incomplete argument: %s\n", argv[i]);
        return 2;
    }
    if (run_path == nullptr) {
        fprintf(stderr,
                "usage: %s agent --config <run> [--fake-script <file>] "
                "[--process-profile <file>] "
                "[--command-menu <menu.spg>] [--command-mask] "
                "[--model-profile <file>] "
                "[--max-steps N] [--max-repairs N] [--allow-exec] "
                "[--memory-dir <d>]\n"
                "  without --fake-script the real model at the config's "
                "(model ...) path is run and journaled\n",
                argv[0]);
        return 2;
    }

    int                       rc             = 1;
    struct file_buffer        run_text       = {};
    struct file_buffer        policy_text    = {};
    struct file_buffer        scenario_text  = {};
    struct file_buffer        script_text    = {};
    struct file_buffer        exemplars_text = {};
    char                     *policy_path    = nullptr;
    char                     *scenario_path  = nullptr;
    char                     *journal_path   = nullptr;
    char                     *model_path     = nullptr;
    struct spg_journal_writer journal        = {};
    bool                      journal_open   = false;
    struct spg_model_adapter  model          = {};
    bool                      model_open     = false;
    struct spg_mem_store      store          = {};
    bool                      store_open     = false;

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

    static struct spg_fake_response script[AGENT_MAX_SCRIPT];
    size_t                          script_n = 0u;
    if (script_path != nullptr) {
        status = read_file(script_path, &script_text);
        if (status != SPG_OK) {
            fprintf(stderr, "agent: read script failed: %s\n",
                    spg_status_to_string(status));
            goto done;
        }
        script_n = split_script_lines(script_text.data, script_text.n, script,
                                      AGENT_MAX_SCRIPT);
        if (script_n == 0u) {
            fprintf(stderr, "agent: empty --fake-script\n");
            goto done;
        }
    }

    /* Optional few-shot exemplars: concrete filled-in recommendation forms a
     * small model can imitate (context (examples ...) section). */
    if (exemplars_path != nullptr) {
        status = read_file(exemplars_path, &exemplars_text);
        if (status != SPG_OK) {
            fprintf(stderr, "agent: read exemplars failed: %s\n",
                    spg_status_to_string(status));
            goto done;
        }
    }

    status = spg_journal_writer_open(&journal, journal_path);
    if (status != SPG_OK) {
        fprintf(stderr, "agent: open journal failed: %s\n",
                spg_status_to_string(status));
        goto done;
    }
    journal_open = true;

    /* Constrained-decode capability table (#34). Built through the shared
     * helper so `eval` decodes identically (#51) — the two having their own
     * copies is how they ended up on different decoders. Empty unless
     * --constrained. */
    static struct spg_model_capability agent_caps[SPG_MODEL_CAPABILITY_MAX];
    static char                        agent_cap_names[CLI_CAP_NAMES_BYTES];
    const size_t                       agent_caps_n =
        constrained ? spg_model_capabilities_from_policy(
                          &policy, policy_text.n, policy_text.data,
                          sizeof agent_cap_names, agent_cap_names,
                          sizeof agent_caps / sizeof agent_caps[0], agent_caps)
                    : 0u;

    /* Best-of-N needs the choice slots to vary between attempts (#2); if the
     * caller asked for it without a temperature, sample. */
    if (best_of > 1u && sample_temp == 0.0f) {
        sample_temp = 0.8f;
    }

    /* --fake-script -> the scripted fake; otherwise the real model at the
     * config's (model ...) path, run and JOURNALED — the production path P6
     * (directive injection) and P7 (recurrence audit) read. */
    /* #56: the menu the model is told about. A file replaces the built-in
     * table for this run; both are proposal spaces, never permissions. */
    static struct spg_cmd_menu   agent_menu;
    static char                  agent_tools[4096];
    static const char           *agent_menu_names[SPG_CMD_MENU_MAX];
    size_t                       agent_menu_n = 0u;
    struct spg_host_info         agent_host   = {};
    (void)spg_host_probe(&agent_host);
    if (menu_path != nullptr) {
        struct file_buffer mt = {};
        if (read_file(menu_path, &mt) != SPG_OK) {
            fprintf(stderr, "agent: cannot read command menu %s\n", menu_path);
            goto done;
        }
        const enum spg_status ms = spg_cmd_menu_load(mt.n, mt.data, &agent_menu);
        free_file_buffer(&mt);
        if (ms != SPG_OK) {
            fprintf(stderr, "agent: invalid command menu %s: %s\n", menu_path,
                    spg_status_to_string(ms));
            goto done;
        }
        (void)spg_cmd_menu_render_of(&agent_menu, sizeof agent_tools, agent_tools);
        agent_menu_n =
            spg_cmd_menu_names(&agent_menu, SPG_CMD_MENU_MAX, agent_menu_names);
    } else {
        (void)spg_cmd_menu_render(agent_host.os, sizeof agent_tools, agent_tools);
        agent_menu_n = spg_cmd_menu_builtin_names(agent_host.os,
                                                  SPG_CMD_MENU_MAX,
                                                  agent_menu_names);
    }

    const struct spg_model_adapter_config model_config =
        script_path != nullptr
            ? (struct spg_model_adapter_config){
                  .kind                = SPG_MODEL_ADAPTER_FAKE,
                  .sampling            = {.top_p = 1.0f},
                  .fake_response_count = script_n,
                  .fake_responses      = script,
              }
            : (struct spg_model_adapter_config){
                  .kind             = SPG_MODEL_ADAPTER_GEIST,
                  .model_path       = model_path,
                  .force_prefix     = constrained ? "(recommend (kind " : nullptr,
                  .capabilities     = agent_caps,
                  .capability_count = agent_caps_n,
                  .command_names = command_mask ? agent_menu_names : nullptr,
                  .command_name_count = command_mask ? agent_menu_n : 0u,
                  .sampling         = {.max_seq_len = 4096u,
                                       .temperature = sample_temp,
                                       .top_p       = 1.0f,
                                       .random_seed =
                                           has_seed ? seed_override : run.seed},
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
    static char                           context[CLI_CONTEXT_BYTES];
    /* #54: scratch for the chat-framed prompt. Room for the context plus the
     * turn markers; a template that would not fit falls back to the bare
     * prompt rather than sending half a format. */
    static char                   framed[CLI_CONTEXT_BYTES + 256u];
    static char                   model_output[CLI_MODEL_OUTPUT_BYTES];
    static struct spg_sexpr_token rec_tokens[CLI_TOKEN_CAPACITY];
    static struct spg_sexpr_node  rec_nodes[CLI_NODE_CAPACITY];
    static char                   policy_payload[CLI_PAYLOAD_BYTES];
    static char                   sim_payload[CLI_PAYLOAD_BYTES];
    static char                   observation[AGENT_OBS_BYTES];
    static char                   shell_stdout[AGENT_SHELL_STDOUT];
    static char                   shell_stderr[AGENT_SHELL_STDERR];
    static char                   mem_index[AGENT_OBS_BYTES];
    static struct spg_journal_record_header trajectory[256];

    const struct spg_agent_run_workspace ws = {
        .context_capacity = sizeof context,
        .context          = context,
        /* #54: scratch for the chat-framed prompt. Sized like the context plus
         * the markers; a template that would not fit falls back to the bare
         * prompt rather than sending half a format. */
        .framed_capacity         = sizeof framed,
        .framed                  = framed,
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
    char *goal_cstr = nullptr; /* materialized (goal "...") span, or null */
    if (run.has_goal) {
        (void)span_to_cstr(run_text.n, run_text.data, run.goal, &goal_cstr);
    }
    /* Sampled once per run, not per tick: phase 3 only proves the state
     * reaches the context. Re-observing after an action is phase 7's job, and
     * it needs an action to observe first. The timestamp is the run's, from the
     * same injected counter the journal uses — no clock is read here either. */
    struct spg_machine_state   machine = {};
    /* #54 for the live agent: eval could describe HOW to speak to a model,
     * the agent could not — so every agent run spoke auto-detect, which for
     * a base model like BitNet means `none` (phase 12: 1/9 parses). Same
     * file format, same loader, same field the eval path fills. */
    static struct spg_model_profile model_profile;
    model_profile = (struct spg_model_profile){};
    if (model_profile_path != nullptr) {
        struct file_buffer mtext = {};
        if (read_file(model_profile_path, &mtext) != SPG_OK) {
            fprintf(stderr, "agent: cannot read model profile: %s\n",
                    model_profile_path);
            return 2;
        }
        const enum spg_status ms = spg_model_profile_load(
            mtext.n, mtext.data, CLI_TOKEN_CAPACITY, rec_tokens,
            CLI_NODE_CAPACITY, rec_nodes, &model_profile);
        free_file_buffer(&mtext);
        if (ms != SPG_OK) {
            fprintf(stderr, "agent: invalid model profile %s: %s\n",
                    model_profile_path, spg_status_to_string(ms));
            return 2;
        }
    }
    struct spg_process_profile profile = {};
    if (profile_path != nullptr) {
        struct file_buffer profile_text = {};
        if (read_file(profile_path, &profile_text) != SPG_OK) {
            fprintf(stderr, "agent: cannot read process profile: %s\n",
                    profile_path);
            return 2;
        }
        struct spg_process_profile_error perr    = {};
        const enum spg_status            pstatus = spg_process_profile_load(
            profile_text.n, profile_text.data, CLI_TOKEN_CAPACITY, rec_tokens,
            CLI_NODE_CAPACITY, rec_nodes, &profile, &perr);
        free_file_buffer(&profile_text);
        if (pstatus != SPG_OK) {
            fprintf(stderr,
                    "agent: invalid process profile %s: %s at offset %zu\n",
                    profile_path, spg_status_to_string(pstatus), perr.offset);
            return 2;
        }
    }
    static struct spg_machine_pause_ledger      pause_ledger;
    char                                        machine_payload[1024];
    const struct spg_machine_executor_workspace release_ws = {
        .payload_capacity = sizeof machine_payload, .payload = machine_payload};
    /* Perception is not opt-in: the host is observed on every run. What stays
     * opt-in is authority — managing processes needs a profile, and the gate
     * needs the capability. The lock, the recovery pass and the release in
     * `done` ride along unconditionally for the same reason: they protect the
     * journal and the host, not a feature flag. */
    {
        machine_lock = lock_machine_journal(journal_path);
        if (machine_lock < 0) {
            fprintf(stderr,
                    "agent: another machine run holds %s — give this one its "
                    "own journal\n",
                    journal_path);
            rc = 2;
            goto done;
        }
        install_interrupt_handler();
        /* Second line of defence first: a previous run may have been killed
         * between a pause and its resume, and that process is still stopped
         * right now. Recover before observing, so the snapshot this run reasons
         * about is not one we ourselves left broken. */
        const struct spg_machine_executor_config recover_cfg = {
            .actor_id          = 1u,
            .timestamp_ns      = 1u,
            .write_journal     = false,
            .execution_enabled = true,
        };
        size_t recovered = 0u;
        if (spg_machine_recover_journal(journal_path, &recover_cfg, &release_ws,
                                        nullptr, &recovered) == SPG_OK &&
            recovered > 0u) {
            printf("recovered=%zu (processes left paused by an earlier run)\n",
                   recovered);
        }
    }
    /* A host that cannot read itself is something the agent is told about —
     * the block renders `unknown` — never a reason to refuse to start. Same
     * line spg_device_sample draws for the plant. */
    if (spg_machine_sample_with_processes(
            1u, nullptr, 0u, nullptr,
            profile_path != nullptr ? &profile : nullptr, &machine) != SPG_OK) {
        fprintf(stderr, "agent: host telemetry unavailable; the machine block"
                        " renders unknown\n");
    }
    if (device.n_channels > 0u) {
        /* No connect step: the first sample IS the connectivity probe. A
         * channel table is the intent; the programs answer or render unknown.
         *
         * Armed against the same injected clock the ticks use — a step
         * counter starting at 1 — so a replay reaches the same watchdog
         * verdict the live run did. */
        spg_device_arm_watchdog(&device, device_watchdog_steps, 0u);
        /* One reading before the first tick, so the first decision is made
         * about a plant the agent has actually seen. The loop re-samples after
         * every action; without this the first context would carry no
         * (device-state ...) block at all and the opening move would be blind.
         *
         * A failing sample is not fatal: the channels that answered are
         * installed and the rest render `unknown`. A plant that cannot be read
         * is something the agent should be told about, not a reason to refuse
         * to start. */
        (void)spg_device_sample(&device, &device_state);
    }
    const struct spg_agent_run_inputs inputs = {
        .model         = &model,
        .policy        = &policy,
        .policy_text_n = policy_text.n,
        .policy_text   = policy_text.data,
        .run           = &run,
        .sim           = &sim,
        .store         = store_open ? &store : nullptr,
        .journal       = &journal,
        .exemplars     = exemplars_text.data, /* null when --exemplars absent */
        .goal          = goal_cstr,           /* null when (goal ...) absent */
        .tools         = agent_tools,
        /* Perception is automatic: every live run sees the host it runs on,
         * unknown fields included. Only scripted worlds (eval fixtures)
         * describe their state instead of measuring it — that path never
         * comes through here. */
        .machine = &machine,
        /* Phase 7: re-observe between ticks. Without this the agent decides
         * every tick on the snapshot it started with — the loop was closed in
         * the eval harness and open in the real agent, which is precisely the
         * gap only a real experiment could show. */
        .refresh_machine   = true,
        .machine_settle_ms = settle_ms,
        /* No profile means nothing is managed, and the gate denies every
         * machine action as unmanaged. That is the right default: a run that
         * never declared what it may touch may not touch anything. */
        .profile      = profile_path != nullptr ? &profile : nullptr,
        .pause_ledger  = &pause_ledger,
        .profile_model = model_profile.present ? &model_profile : nullptr,
        .device       = device.n_channels > 0u ? &device : nullptr,
        .device_state = device.n_channels > 0u ? &device_state : nullptr,
    };
    const struct spg_agent_run_config rcfg = {
        .max_steps   = max_steps,
        .max_repairs = max_repairs,
        /* #40: a model that acts validly but never emits (kind finish) would
         * run to the step cap; treat a converged (no-progress) run as done so
         * it terminates FINISHED and an expect verdict can pass. The model
         * profile may override: a controller that must stay on a plant cannot
         * be one the loop sends home early (see model_profile.h). */
        .finish_on_no_progress =
            model_profile.present && model_profile.has_finish_on_no_progress
                ? model_profile.finish_on_no_progress
                : true,
        .directive_slug        = directive_slug,
        .execution_enabled     = allow_exec,
        .exec_timeout_ms       = 5000u,
        .exec_stdout_cap       = sizeof shell_stdout,
        .exec_stderr_cap       = sizeof shell_stderr,
        .context_refs          = CLI_CONTEXT_REFS,
    };
    struct spg_policy_usage      usage       = {};
    struct spg_agent_loop_result loop_result = {};

    /* Optional success criterion (docs/LEARNING.md P1): judge the real run the
     * same way the eval loop judges a scripted case — model-free, zero tokens.
     */
    const bool have_expect =
        run.has_expect && run.expect_observation.length < sizeof observation;
    char                   expect_obs[AGENT_OBS_BYTES];
    struct spg_eval_expect expect = {};
    if (have_expect) {
        memcpy(expect_obs, run_text.data + run.expect_observation.offset,
               run.expect_observation.length);
        expect_obs[run.expect_observation.length] = '\0';
        expect =
            (struct spg_eval_expect){.check_termination = true,
                                     .termination = SPG_AGENT_LOOP_FINISHED,
                                     .observation = expect_obs};
    }

    /* Best-of-N (#2, fixed in #55): up to best_of attempts, model loaded once;
     * the choice-slot RNG drifts between attempts (temperature > 0), so each
     * explores different valid decisions.
     *
     * Selection used to require an (expect ...) — the ANSWER — so without one
     * the feature silently collapsed to a single attempt and was off in
     * production. It now ranks attempts with spg_run_rank, which needs no
     * oracle. A declared (expect ...) still wins when present: an attempt that
     * satisfies it is unbeatable and ends the loop immediately.
     *
     * Ties are NOT broken on fewer steps, tempting as it looks. Without an
     * expectation a one-step run cannot be told from a model that emitted
     * `finish` immediately and did nothing, so preferring brevity would
     * actively reward the degenerate answer. On a tie the first attempt wins:
     * deterministic, and it invents no preference the data does not support. */
    const size_t          attempts = best_of;
    enum spg_eval_outcome verdict  = SPG_EVAL_PASS;
    size_t                used     = 0u;
    size_t                chosen   = 1u;   /* 1-based attempt that won */
    int                   best_rank = -2;  /* below every real rank */
    struct spg_agent_loop_result best_loop   = {};
    enum spg_status              best_status = SPG_E_INVALID_STATE;
    static char                  best_obs[AGENT_OBS_BYTES];
    best_obs[0] = '\0';
    for (size_t a = 0u; a < attempts; a += 1u) {
        used   = a + 1u;
        usage  = (struct spg_policy_usage){}; /* each attempt is independent */
        status = spg_agent_run(&inputs, &rcfg, &ws, &usage, &loop_result);

        const enum spg_eval_outcome attempt_verdict =
            have_expect ? spg_eval_judge(&expect, &loop_result, status, observation)
                        : SPG_EVAL_PASS;
        /* A satisfied expectation outranks every answer-free rung. */
        const int rank = (have_expect && attempt_verdict == SPG_EVAL_PASS)
                             ? 100
                             : spg_run_rank(status, loop_result.termination);
        if (rank > best_rank) {
            best_rank   = rank;
            best_loop   = loop_result;
            best_status = status;
            verdict     = attempt_verdict;
            chosen      = used;
            (void)snprintf(best_obs, sizeof best_obs, "%s", observation);
        }
        if (rank == 100) {
            break; /* cannot be improved on */
        }
        if (!have_expect && rank >= 4) {
            break; /* top answer-free rung: finished on its own */
        }
    }
    loop_result = best_loop;
    status      = best_status;
    (void)snprintf(observation, sizeof observation, "%s", best_obs);

    printf("steps=%zu termination=%s journal=%s\n", loop_result.steps_taken,
           spg_agent_loop_termination_to_string(loop_result.termination),
           journal_path);
    if (observation[0] != '\0') {
        printf("observation: %s\n", observation);
    }
    rc = (status == SPG_OK) ? 0 : 1;

    /* best_of reporting is no longer conditional on an expectation: the whole
     * point of #55 is that selection happens without one. */
    if (attempts > 1u) {
        printf("best_of=%zu attempts_used=%zu chosen=%zu rank=%d\n", attempts,
               used, chosen, best_rank);
    }
    if (have_expect) {
        printf("verdict=%s\n", spg_eval_outcome_to_string(verdict));
        /* a FAIL exits non-zero so a caller (and a future miner) can act on it */
        if (verdict != SPG_EVAL_PASS && rc == 0) {
            rc = 1;
        }
    }

done:
    /* Nothing stays paused. Every path lands here — success, failure, max
     * steps, budget, policy denial, interrupt — so the promise does not depend
     * on how the run ended. An untouched ledger releases nothing. */
    {
        const struct spg_machine_executor_state release_state = {
            .machine = &machine,
            .journal = journal_open ? &journal : nullptr,
            .ledger  = &pause_ledger,
        };
        const struct spg_machine_executor_config release_cfg = {
            .actor_id          = 1u,
            .timestamp_ns      = 1u,
            .write_journal     = journal_open,
            .execution_enabled = true,
        };
        size_t resumed = 0u;
        (void)spg_machine_ledger_release(&pause_ledger, &release_state,
                                         &release_cfg, &release_ws, &resumed);
        if (resumed > 0u) {
            printf("released=%zu%s\n", resumed,
                   interrupt_requested ? " (interrupted)" : "");
        }
    }
    if (machine_lock >= 0) {
        /* Released after the pauses are: a second run must not start recovery
         * while this one is still handing processes back. */
        (void)close(machine_lock);
    }
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
    free_file_buffer(&exemplars_text);
    return rc;
}

/* ---- eval suite runner ---- */

/* Find a (name ...) child-list directly under parent; INVALID if absent. */
static uint32_t eval_field(const struct spg_sexpr_node *nodes,
                           const uint32_t parent, const size_t in_n,
                           const char *in, const char *name) {
    for (uint32_t c                      = spg_sexpr_first_child(nodes, parent);
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
    const uint32_t       v = spg_sexpr_second_child(nodes, f);
    struct spg_text_span sp;
    if (v == SPG_SEXPR_INVALID_INDEX ||
        !spg_sexpr_string_payload_span(&nodes[v], &sp) ||
        sp.length + 1u > cap) {
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
    if (v == SPG_SEXPR_INVALID_INDEX ||
        nodes[v].kind != SPG_SEXPR_NODE_SYMBOL) {
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
                             const char                      *in,
                             enum spg_agent_loop_termination *out) {
    const uint32_t f = eval_field(nodes, expect, in_n, in, "termination");
    if (f == SPG_SEXPR_INVALID_INDEX) {
        return false;
    }
    const uint32_t v = spg_sexpr_second_child(nodes, f);
    if (v == SPG_SEXPR_INVALID_INDEX ||
        nodes[v].kind != SPG_SEXPR_NODE_SYMBOL) {
        return false;
    }
    static const enum spg_agent_loop_termination all[] = {
        SPG_AGENT_LOOP_FINISHED, SPG_AGENT_LOOP_MAX_STEPS,
        SPG_AGENT_LOOP_REJECTED, SPG_AGENT_LOOP_DENIED,
        SPG_AGENT_LOOP_BUDGET,   SPG_AGENT_LOOP_ERROR};
    for (size_t i = 0u; i < sizeof all / sizeof all[0]; i += 1u) {
        if (spg_sexpr_span_eq_cstr(
                in_n, in, nodes[v].span,
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
    size_t total;  /* individual runs (cases x samples)   */
    size_t passed; /* individual passing runs             */
    size_t ncases; /* distinct cases (index into the rest) */
    char   names[EVAL_MAX_CASES][64];
    struct spg_eval_case_result results[EVAL_MAX_CASES]; /* last sample/case */
    size_t                      runs[EVAL_MAX_CASES];    /* samples per case */
    size_t case_passed[EVAL_MAX_CASES];                  /* k passed of N    */
    /* #53 ladder, per case and summed. Monotone by construction:
     * task <= gate <= parse. A pass-rate alone cannot tell "the model cannot
     * produce the form" from "it produces a valid form and picks the wrong
     * action", and those want opposite fixes. */
    size_t case_parsed[EVAL_MAX_CASES];
    size_t case_gated[EVAL_MAX_CASES];
    /* Phase 10 (#70): wall time per case and peak RSS for the whole suite.
     *
     * Measured here and nowhere else. The runtime reads no clock — that is what
     * keeps replay byte-identical — but a benchmark that cannot say how long
     * something took is not a benchmark. This never enters a journal or a
     * context; it is the harness observing itself. */
    uint64_t case_latency_ms[EVAL_MAX_CASES];
    uint64_t suite_latency_ms;
    uint64_t peak_rss_kb;
    /* Off by default, and that is not laziness: two identical eval runs must
     * produce byte-identical reports, and a wall-clock number in the default
     * output breaks every diff and every fixture test that relies on it.
     * A measurement belongs behind a switch; a report is for comparing. */
    bool report_timing;
    /* Phase 9: the objective verdict per case, reported alongside the outcome
     * so a reader can see WHY a run that finished cleanly still failed. */
    enum spg_goal_verdict goal_verdicts[EVAL_MAX_CASES];
    size_t                parsed; /* runs whose form was accepted        */
    size_t                gated;  /* ... and the policy gate allowed it  */

    /* #64 diagnosis metrics. A diagnosis case asks one question — what is
     * wrong — so a bare pass rate hides the two failures that matter: naming
     * the wrong cause, and proposing an action when none was asked for. */
    bool diagnosis_suite; /* any case declared (expect (diagnosis ...)) */
    char case_expected[EVAL_MAX_CASES][32];
    char case_emitted[EVAL_MAX_CASES][32]; /* last sample's category */
    /* The reason verbatim. A metric nobody can audit is a metric nobody
     * should trust: the benchmark shows what the model actually wrote. */
    char   case_reason[EVAL_MAX_CASES][96];
    size_t case_diag_ok[EVAL_MAX_CASES]; /* samples naming the right cause */
    size_t case_halluc[EVAL_MAX_CASES];  /* ... naming a process not present */
    size_t case_action[EVAL_MAX_CASES];  /* ... proposing an action anyway */
    size_t case_ctx_bytes[EVAL_MAX_CASES];
    bool   case_heldout[EVAL_MAX_CASES];
};

/* Diagnosis convention: (recommend (kind finish) (reason "<category> [<id>]")).
 * `finish` carries no capability, consumes no budget and never reaches the
 * policy gate, so a pure-diagnosis run needs no new action kind and no
 * executor — the reason IS the output, and it is journalled like any other. */
static void diagnosis_from_output(const char *text, char category[static 32],
                                  char process[static 32]) {
    category[0] = '\0';
    process[0]  = '\0';
    if (text == nullptr) {
        return;
    }
    const char *reason = strstr(text, "(reason \"");
    if (reason == nullptr) {
        return;
    }
    reason += sizeof "(reason \"" - 1u;
    size_t i = 0u;
    while (i + 1u < 32u && reason[i] != '\0' && reason[i] != '"' &&
           reason[i] != ' ') {
        category[i] = reason[i];
        i += 1u;
    }
    category[i] = '\0';
    /* Gemma answered "healthy))" — the category is there, the model just kept
     * closing parens inside the string. Scoring that as "no diagnosis" would
     * measure punctuation, not reasoning. */
    while (i > 0u && (category[i - 1u] == ')' || category[i - 1u] == '.' ||
                      category[i - 1u] == ',')) {
        i -= 1u;
        category[i] = '\0';
    }
    /* The closed set lives in diagnose.h, shared with the rule baseline: one
     * definition, so a category cannot exist for the rules and not for the
     * scorer. */
    bool known = false;
    (void)spg_diagnosis_from_string(category, &known);
    if (!known) {
        category[0] = '\0'; /* unrecognised -> no diagnosis, not a wrong one */
        return;
    }
    (void)process;
}

/* Every token after the category, stripped of the punctuation a model wraps
 * lists in. Returns false when there are no more.
 *
 * The first version of this read only the SECOND token and treated it as an
 * id. Gemma writes `memory_pressure [critical_app, batch_job]`, so that
 * counted "[critical_app," as an invented process and reported an 8/9
 * hallucination rate for a model that had named two processes that were both
 * present. Measuring the punctuation instead of the claim is worse than not
 * measuring at all. */
static bool next_reason_token(const char **cursor, char token[static 32]) {
    const char *p = *cursor;
    while (*p == ' ' || *p == '[' || *p == ']' || *p == ',' || *p == '(' ||
           *p == ')') {
        p += 1;
    }
    if (*p == '\0' || *p == '"') {
        return false;
    }
    size_t n = 0u;
    while (n + 1u < 32u && p[n] != '\0' && p[n] != '"' && p[n] != ' ' &&
           p[n] != ',' && p[n] != ']' && p[n] != '[' && p[n] != ')') {
        token[n] = p[n];
        n += 1u;
    }
    token[n] = '\0';
    *cursor  = p + n;
    return n > 0u;
}

/* A named process that is not in the snapshot is invented — the sharpest
 * hallucination signal available without a grader. */
static bool
diagnosis_names_absent_process(const struct spg_machine_state *state,
                               const char                     *reason_text) {
    if (state == nullptr || reason_text == nullptr) {
        return false;
    }
    const char *cursor = strstr(reason_text, "(reason \"");
    if (cursor == nullptr) {
        return false;
    }
    cursor += sizeof "(reason \"" - 1u;
    char token[32];
    bool first = true;
    while (next_reason_token(&cursor, token)) {
        if (first) {
            first = false; /* the category, not a process claim */
            continue;
        }
        /* Only tokens that claim to be an id are judged. Process ids carry an
         * underscore by convention (critical_app, batch_job); prose does not,
         * and a model is allowed to write prose. */
        if (strchr(token, '_') == nullptr) {
            continue;
        }
        bool present = false;
        for (size_t i = 0u; i < state->n_processes; i += 1u) {
            present = present ||
                      strcmp(state->processes[i].profile_id, token) == 0 ||
                      strcmp(state->processes[i].name, token) == 0;
        }
        if (!present) {
            return true;
        }
    }
    return false;
}

/* Score one diagnosis run. Everything here is derived from what the run
 * actually emitted — no grader, no second model judging the first. */
static bool eval_tally_diagnosis(struct eval_run_report *report,
                                 const size_t case_idx, const char *expected,
                                 const char                     *model_output,
                                 const struct spg_machine_state *state,
                                 const struct spg_agent_loop_result *loop) {
    char category[32];
    char process[32];
    diagnosis_from_output(model_output, category, process);
    const char *reason =
        model_output != nullptr ? strstr(model_output, "(reason \"") : nullptr;
    if (reason != nullptr) {
        reason += sizeof "(reason \"" - 1u;
        size_t i = 0u;
        while (i + 1u < sizeof report->case_reason[0] && reason[i] != '\0' &&
               reason[i] != '"') {
            report->case_reason[case_idx][i] = reason[i];
            i += 1u;
        }
        report->case_reason[case_idx][i] = '\0';
    }
    (void)snprintf(report->case_emitted[case_idx],
                   sizeof report->case_emitted[0], "%s",
                   category[0] != '\0' ? category : "-");
    if (category[0] != '\0' && strcmp(category, expected) == 0) {
        report->case_diag_ok[case_idx] += 1u;
    }
    if (diagnosis_names_absent_process(state, model_output)) {
        report->case_halluc[case_idx] += 1u;
    }
    /* A diagnosis needs exactly one step: emit finish. More means the model
     * proposed something to DO — which nobody asked for, and which phase 6
     * would have to deny. */
    if (loop->steps_taken > 1u) {
        report->case_action[case_idx] += 1u;
    }
    return category[0] != '\0' && strcmp(category, expected) == 0;
}

/* One run's rungs. REJECTED means the loop ran out of repairs on a malformed
 * reply — the form never parsed. DENIED means it parsed and the policy gate
 * refused it. Anything else got past both, whether or not it reached the goal.
 */
static void eval_tally_ladder(struct eval_run_report            *report,
                              const size_t                       case_idx,
                              const struct spg_eval_case_result *r) {
    if (r->outcome == SPG_EVAL_FAIL_RUN_ERROR) {
        return; /* the harness failed, not the model — no rung is earned */
    }
    if (r->termination == SPG_AGENT_LOOP_REJECTED) {
        return;
    }
    report->case_parsed[case_idx] += 1u;
    report->parsed += 1u;
    if (r->termination == SPG_AGENT_LOOP_DENIED) {
        return;
    }
    report->case_gated[case_idx] += 1u;
    report->gated += 1u;
}

/* Per-invocation knobs. A null pointer (or zeroed struct) reproduces the
 * historical behaviour: one sample per case, no remote endpoint. */
struct eval_run_opts {
    uint32_t    ablate; /* phase 11: parts of the snapshot to withhold */
    const char *profile_model_path; /* #54: how to frame the prompt */
    const char *remote_url; /* nullable: enables (model "remote") cases      */
    const char *remote_model; /* nullable: model name for remote cases */
    size_t      samples; /* 0/1 => run each case once                     */
    /* #51: run (model "geist") cases through the SAME decoder as
     * `agent --constrained`. Off by default so the shipped scripted-fake
     * suites keep their recorded free-decode behaviour. */
    bool constrained;
    /* #51: sampling temperature for real-model cases. 0.0 (the default) is
     * greedy — which makes --samples N produce N identical runs, so a suite
     * that wants variance must ask for it. */
    float temperature;
};

/* #52: one case-sample's sandbox. Big (the store carries an index cache), so
 * the single instance lives in static storage at the call site. */
struct eval_sandbox {
    char                 dir[CLI_PATH_MAX];
    struct spg_mem_store store;
    bool                 store_open;
};

#define EVAL_SANDBOX_ROOT "build/eval"

/* Rebuild sb for (case_name, sample): a pristine copy of fixture_dir, with the
 * shared mind-palace overlaid into <dir>/mem when there is one.
 *
 * The overlay is what lets `improve` keep working: its candidate lesson lives
 * in the shared store, and a case that simply ignored it would make the mint
 * gate measure a run that never saw the lesson. Copying instead of sharing
 * means the agent can also *write* memory without leaking into the next sample.
 *
 * Returns false if any step fails; the caller scores that sample as a run
 * error rather than reporting a result measured against a dirty directory. */
static bool eval_sandbox_prepare(struct eval_sandbox *sb, const char *case_name,
                                 const size_t sample, const char *fixture_dir,
                                 const struct spg_mem_store *shared) {
    sb->store_open = false;
    if (spg_fixture_sample_dir(EVAL_SANDBOX_ROOT, case_name, sample,
                               sizeof sb->dir, sb->dir) != SPG_OK ||
        spg_fixture_reset(sb->dir) != SPG_OK ||
        spg_fixture_copy_into(sb->dir, fixture_dir) != SPG_OK) {
        return false;
    }
    char      mem[CLI_PATH_MAX];
    const int n = snprintf(mem, sizeof mem, "%s/mem", sb->dir);
    if (n < 0 || (size_t)n >= sizeof mem) {
        return false;
    }
    /* The store creates the directory, so it exists before the overlay. */
    if (spg_mem_store_open(&sb->store, mem) != SPG_OK) {
        return false;
    }
    if (shared != nullptr &&
        spg_fixture_copy_into(mem, shared->dir) != SPG_OK) {
        return false;
    }
    sb->store_open = true;
    return true;
}

/* Load a suite + its shared run/policy/scenario config and run every case with
 * the given mind-palace store (nullable) threaded into each run; fill *report.
 * Returns SPG_OK on a complete run. Prints nothing (the caller reports). */
static enum spg_status eval_run_suite(const char                 *suite_path,
                                      struct spg_mem_store       *store,
                                      const struct eval_run_opts *opts,
                                      struct eval_run_report     *report) {
    *report                          = (struct eval_run_report){};
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
                             tok, CLI_NODE_CAPACITY, nod, &tn, &nn,
                             &se) != SPG_OK ||
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
    status                    = load_run_file(config_path, &run_text, &run);
    if (status == SPG_OK) {
        status = span_to_cstr(run_text.n, run_text.data, run.policy_path,
                              &policy_path);
    }
    if (status == SPG_OK) {
        status = span_to_cstr(run_text.n, run_text.data, run.scenario_path,
                              &scenario_path);
    }
    if (status == SPG_OK) {
        status = span_to_cstr(run_text.n, run_text.data, run.model_path,
                              &model_path);
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

    /* #51: the constrained decoder's capability mask, built through the same
     * helper the agent uses. Both buffers must outlive every adapter built
     * below, hence static. */
    /* #56: one menu for the whole suite — the same one the agent path
     * renders, so a case does not see a different world than a live run. */
    static char          eval_tools[4096];
    static const char   *eval_menu_names[SPG_CMD_MENU_MAX];
    struct spg_host_info eval_host = {};
    (void)spg_host_probe(&eval_host);
    (void)spg_cmd_menu_render(eval_host.os, sizeof eval_tools, eval_tools);
    const size_t eval_menu_n =
        spg_cmd_menu_builtin_names(eval_host.os, SPG_CMD_MENU_MAX, eval_menu_names);

    static struct spg_model_capability eval_caps[SPG_MODEL_CAPABILITY_MAX];
    static char                        eval_cap_names[CLI_CAP_NAMES_BYTES];
    const size_t eval_caps_n = spg_model_capabilities_from_policy(
        &policy, policy_text.n, policy_text.data, sizeof eval_cap_names,
        eval_cap_names, sizeof eval_caps / sizeof eval_caps[0], eval_caps);

    static struct spg_context_graph_ref   graph_refs[CLI_CONTEXT_REFS];
    static struct spg_context_memory_ref  memory_refs[CLI_CONTEXT_REFS];
    static struct spg_context_journal_ref journal_refs[CLI_CONTEXT_REFS];
    static char                           context[CLI_CONTEXT_BYTES];
    /* #54: scratch for the chat-framed prompt. Room for the context plus the
     * turn markers; a template that would not fit falls back to the bare
     * prompt rather than sending half a format. */
    static char                   framed[CLI_CONTEXT_BYTES + 256u];
    static char                   model_output[CLI_MODEL_OUTPUT_BYTES];
    static struct spg_sexpr_token rtok[CLI_TOKEN_CAPACITY];
    static struct spg_sexpr_node  rnod[CLI_NODE_CAPACITY];
    static char                   ppay[CLI_PAYLOAD_BYTES];
    static char                   spay[CLI_PAYLOAD_BYTES];
    static char                   observation[AGENT_OBS_BYTES];
    static char                   sh_out[AGENT_SHELL_STDOUT];
    static char                   sh_err[AGENT_SHELL_STDERR];
    static char                   mem_index[AGENT_OBS_BYTES];
    static struct spg_journal_record_header traj[256];
    const struct spg_agent_run_workspace    ws = {
        .context_capacity = sizeof context,
        .context          = context,
        /* #54: scratch for the chat-framed prompt. Sized like the context plus
         * the markers; a template that would not fit falls back to the bare
         * prompt rather than sending half a format. */
        .framed_capacity         = sizeof framed,
        .framed                  = framed,
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
        .tools         = eval_tools,
    };

    /* Invocation-wide knobs (resolved once, not per case). */
    const struct eval_run_opts  defaults = {};
    const struct eval_run_opts *o        = (opts != nullptr) ? opts : &defaults;
    const size_t                samples  = (o->samples > 0u) ? o->samples : 1u;
    const char                 *env_api_url = getenv("GEISTSHELL_API_URL");
    const char                 *remote_url =
        (o->remote_url != nullptr && o->remote_url[0] != '\0') ? o->remote_url
        : (env_api_url != nullptr && env_api_url[0] != '\0')   ? env_api_url
                                                               : nullptr;
    const char *api_key = getenv("GEISTSHELL_API_KEY");

    /* #54: loaded once for the whole suite. A profile is the run's provenance —
     * a benchmark number without the profile that produced it cannot be
     * reproduced, which is why this is a file and not a set of flags. */
    static struct spg_model_profile model_profile;
    model_profile = (struct spg_model_profile){};
    if (o->profile_model_path != nullptr) {
        struct file_buffer ptext = {};
        if (read_file(o->profile_model_path, &ptext) != SPG_OK) {
            fprintf(stderr, "eval: cannot read model profile %s\n",
                    o->profile_model_path);
            rc = SPG_E_IO;
            goto done;
        }
        const enum spg_status ms = spg_model_profile_load(
            ptext.n, ptext.data, ws.token_capacity, ws.tokens, ws.node_capacity,
            ws.nodes, &model_profile);
        free_file_buffer(&ptext);
        if (ms != SPG_OK) {
            fprintf(stderr, "eval: invalid model profile %s: %s\n",
                    o->profile_model_path, spg_status_to_string(ms));
            rc = ms;
            goto done;
        }
        fprintf(stderr, "eval: model profile %s (template %s)\n",
                model_profile.name,
                spg_chat_template_to_string(
                    model_profile.chat_template == SPG_TEMPLATE_AUTO
                        ? spg_template_for_arch(model_profile.arch)
                        : model_profile.chat_template));
    }

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
        const size_t   case_idx   = report->ncases;
        const uint64_t case_start = bench_now_ms();
        char          *name       = report->names[case_idx];
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
        const uint32_t         exp =
            eval_field(nod, c, suite_text.n, suite_text.data, "expect");
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

        /* #52: a case that declares a (fixture ...) runs inside a sandbox,
         * rebuilt from the pristine fixture before EVERY sample. Without it a
         * stateful case finds its own previous mutation and passes without
         * performing the action under test. No fixture -> the historical
         * behaviour (workdir ".", the shared store), byte for byte. */
        /* #53: a real-model baseline needs a task per case; without this every
         * case would inherit the one goal in the shared run config. */
        static char case_goal[1024];
        const bool  has_goal = eval_str(nod, c, suite_text.n, suite_text.data,
                                        "goal", case_goal, sizeof case_goal);
        char        fixture[CLI_PATH_MAX];
        const bool has_fixture = eval_str(nod, c, suite_text.n, suite_text.data,
                                          "fixture", fixture, sizeof fixture);

        /* #64: the scenario is a machine state. Reading it from a file rather
         * than sampling the host is what makes a diagnosis case reproducible
         * on a laptop with no Pi attached. */
        static struct spg_machine_state        case_machine;
        static struct spg_machine_pause_ledger case_pause_ledger;
        char                                   machine_path[CLI_PATH_MAX];
        bool                                   has_machine =
            eval_str(nod, c, suite_text.n, suite_text.data, "machine",
                     machine_path, sizeof machine_path);
        if (has_machine) {
            struct file_buffer mtext = {};
            if (read_file(machine_path, &mtext) != SPG_OK) {
                fprintf(stderr, "eval: cannot read machine fixture %s\n",
                        machine_path);
                rc = SPG_E_IO;
                goto done;
            }
            const enum spg_status ms = spg_machine_state_parse(
                mtext.n, mtext.data, ws.token_capacity, ws.tokens,
                ws.node_capacity, ws.nodes, &case_machine);
            free_file_buffer(&mtext);
            if (ms != SPG_OK) {
                fprintf(stderr, "eval: invalid machine fixture %s: %s\n",
                        machine_path, spg_status_to_string(ms));
                rc = ms;
                goto done;
            }
            /* What this scenario costs the model's window. Measured from the
             * block itself, so it is the same number on the scripted and the
             * real-model path. */
            char   block[SPG_MACHINE_RENDER_CAP];
            size_t block_n = 0u;
            /* Measured with the SAME mask the model saw, or the ablation
             * table would report the full context size for every variant and
             * the whole experiment would be unfalsifiable. */
            if (spg_machine_state_render_masked(&case_machine, o->ablate,
                                                sizeof block, block,
                                                &block_n) == SPG_OK) {
                report->case_ctx_bytes[case_idx] = block_n - 1u;
            }
        }
        /* Phase 7 (#67): what the machine looks like once an action has run.
         * A closed loop is only a loop if the next tick sees the consequence,
         * and a scripted case needs that consequence to be deterministic. */
        static struct spg_machine_state case_machine_after;
        char                            machine_after_path[CLI_PATH_MAX];
        const bool                      has_machine_after =
            eval_str(nod, c, suite_text.n, suite_text.data, "machine_after",
                     machine_after_path, sizeof machine_after_path);
        if (has_machine_after) {
            struct file_buffer mtext = {};
            if (read_file(machine_after_path, &mtext) != SPG_OK) {
                fprintf(stderr, "eval: cannot read machine_after %s\n",
                        machine_after_path);
                rc = SPG_E_IO;
                goto done;
            }
            const enum spg_status ms = spg_machine_state_parse(
                mtext.n, mtext.data, ws.token_capacity, ws.tokens,
                ws.node_capacity, ws.nodes, &case_machine_after);
            free_file_buffer(&mtext);
            if (ms != SPG_OK) {
                fprintf(stderr, "eval: invalid machine_after %s: %s\n",
                        machine_after_path, spg_status_to_string(ms));
                rc = ms;
                goto done;
            }
        }
        /* Phase 9 (#69): what the run is for. The model reads it, and the
         * harness checks it against the machine afterwards — those are two
         * different things and only the second decides the verdict. */
        static struct spg_machine_goal case_machine_goal;
        char                           goal_path[CLI_PATH_MAX];
        const bool                     has_goal_file =
            eval_str(nod, c, suite_text.n, suite_text.data, "goal_file",
                     goal_path, sizeof goal_path);
        if (has_goal_file) {
            struct file_buffer gtext = {};
            if (read_file(goal_path, &gtext) != SPG_OK) {
                fprintf(stderr, "eval: cannot read goal_file %s\n", goal_path);
                rc = SPG_E_IO;
                goto done;
            }
            const enum spg_status gs = spg_machine_goal_load(
                gtext.n, gtext.data, ws.token_capacity, ws.tokens,
                ws.node_capacity, ws.nodes, &case_machine_goal);
            free_file_buffer(&gtext);
            if (gs != SPG_OK) {
                fprintf(stderr, "eval: invalid goal_file %s: %s\n", goal_path,
                        spg_status_to_string(gs));
                rc = gs;
                goto done;
            }
        } else {
            case_machine_goal = (struct spg_machine_goal){};
        }

        /* Without a profile nothing is managed and every machine action is
         * denied — correct as a default, useless as a scenario. */
        static struct spg_process_profile case_profile;
        char                              profile_case_path[CLI_PATH_MAX];
        const bool                        has_case_profile =
            eval_str(nod, c, suite_text.n, suite_text.data, "process_profile",
                     profile_case_path, sizeof profile_case_path);
        if (has_case_profile) {
            struct file_buffer ptext = {};
            if (read_file(profile_case_path, &ptext) != SPG_OK) {
                fprintf(stderr, "eval: cannot read process_profile %s\n",
                        profile_case_path);
                rc = SPG_E_IO;
                goto done;
            }
            struct spg_process_profile_error perr = {};
            const enum spg_status            ps   = spg_process_profile_load(
                ptext.n, ptext.data, ws.token_capacity, ws.tokens,
                ws.node_capacity, ws.nodes, &case_profile, &perr);
            free_file_buffer(&ptext);
            if (ps != SPG_OK) {
                fprintf(stderr, "eval: invalid process_profile %s: %s\n",
                        profile_case_path, spg_status_to_string(ps));
                rc = ps;
                goto done;
            }
        }
        char       expected_diag[32] = {};
        const bool has_diagnosis =
            exp != SPG_SEXPR_INVALID_INDEX &&
            eval_str(nod, exp, suite_text.n, suite_text.data, "diagnosis",
                     expected_diag, sizeof expected_diag);
        if (has_diagnosis) {
            report->diagnosis_suite = true;
            (void)snprintf(report->case_expected[case_idx],
                           sizeof report->case_expected[0], "%s",
                           expected_diag);
            report->case_heldout[case_idx] =
                eval_flag(nod, c, suite_text.n, suite_text.data, "heldout");
        }
        static struct eval_sandbox sandbox_state;
        const char *const          sandbox = sandbox_state.dir;

        const struct spg_agent_run_config rcfg = {
            .max_steps           = (size_t)max_steps,
            .max_repairs         = (size_t)max_repairs,
            .execution_enabled   = allow_exec,
            .exec_timeout_ms     = 5000u,
            .exec_stdout_cap     = sizeof sh_out,
            .exec_stderr_cap     = sizeof sh_err,
            .context_refs        = CLI_CONTEXT_REFS,
            .exec_working_dir    = has_fixture ? sandbox : nullptr,
            .exec_workdir_prefix = has_fixture ? sandbox : nullptr,
        };
        char       model_kind[16];
        const bool has_model = eval_str(nod, c, suite_text.n, suite_text.data,
                                        "model", model_kind, sizeof model_kind);
        const bool geist_case  = has_model && strcmp(model_kind, "geist") == 0;
        const bool remote_case = has_model && strcmp(model_kind, "remote") == 0;
        /* #65: the rule baseline is not a second scoring path. It computes the
         * answer and then goes through the same agent loop, policy gate and
         * metrics as any model — otherwise the comparison would be between two
         * harnesses rather than two methods. */
        const bool  rules_case = has_model && strcmp(model_kind, "rules") == 0;
        static char rules_script_line[160];
        if (rules_case) {
            if (!has_machine) {
                fprintf(
                    stderr,
                    "eval: (model \"rules\") needs a (machine ...) state\n");
                rc = SPG_E_INVALID_ARG;
                goto done;
            }
            const struct spg_rule_thresholds th = spg_rule_thresholds_default();
            struct spg_diagnosis_result      rd = {};
            if (spg_rule_diagnose(&case_machine, &th, &rd) != SPG_OK) {
                rc = SPG_E_INTERNAL;
                goto done;
            }
            (void)snprintf(rules_script_line, sizeof rules_script_line,
                           "(recommend (kind finish) (reason \"%s%s%s\"))",
                           spg_diagnosis_to_string(rd.diagnosis),
                           rd.process_id[0] != '\0' ? " " : "", rd.process_id);
        }

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
                             .temperature = o->temperature,
                             .top_p       = 1.0f,
                             .top_k       = 0,
                             .random_seed = run.seed},
            };
            if (geist_case) {
                mc.kind       = SPG_MODEL_ADAPTER_GEIST;
                mc.model_path = model_path;
                /* #51: the same decoder `agent --constrained` runs. Without
                 * this the suite measures free decode — a configuration
                 * nobody ships, and the one a tool-less model cannot pass. */
                if (o->constrained) {
                    mc.force_prefix     = "(recommend (kind ";
                    mc.capabilities     = eval_caps;
                    mc.capability_count = eval_caps_n;
                    /* #56: the profile decides whether the command slot is
                     * masked to the menu. Off keeps it free — the arm a
                     * baseline compares against. */
                    if (model_profile.command_mask) {
                        mc.command_names      = eval_menu_names;
                        mc.command_name_count = eval_menu_n;
                    }
                }
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
                    rc = SPG_E_MODEL; /* a configured GGUF that won't load
                                         aborts */
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
                    if (has_goal) {
                        gin.goal = case_goal;
                    }
                    if (has_machine) {
                        gin.machine = &case_machine;
                    }
                    if (has_machine_after) {
                        gin.machine_after   = &case_machine_after;
                        gin.refresh_machine = true;
                    }
                    if (has_case_profile) {
                        gin.profile      = &case_profile;
                        gin.pause_ledger = &case_pause_ledger;
                    }
                    if (model_profile.present) {
                        gin.profile_model = &model_profile;
                    }
                    if (has_goal_file) {
                        gin.machine_goal = &case_machine_goal;
                    }
                    gin.machine_ablate = o->ablate;
                    if (has_fixture) {
                        if (!eval_sandbox_prepare(&sandbox_state, name, s,
                                                  fixture, store)) {
                            last = (struct spg_eval_case_result){
                                .outcome = SPG_EVAL_FAIL_RUN_ERROR,
                                .status  = SPG_E_IO};
                            report->total += 1u;
                            runs_in_case += 1u;
                            continue;
                        }
                        gin.store = &sandbox_state.store;
                    }
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
                    eval_tally_ladder(report, case_idx, &last);
                    if (has_diagnosis &&
                        !eval_tally_diagnosis(
                            report, case_idx, expected_diag, model_output,
                            has_machine ? &case_machine : nullptr, &loop) &&
                        last.outcome == SPG_EVAL_PASS) {
                        /* Terminating cleanly is not the same as being right.
                         * A wrong root cause is a failed expectation, or the
                         * suite's headline number would report agreement it
                         * never measured. */
                        last.outcome = SPG_EVAL_FAIL_OBSERVATION;
                    }
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
            if (!has_script && !rules_case) {
                rc = SPG_E_FORMAT; /* a scripted case needs a script */
                goto done;
            }
            struct file_buffer script_text = {};
            if (!rules_case && read_file(script_path, &script_text) != SPG_OK) {
                rc = SPG_E_IO;
                goto done;
            }
            static struct spg_fake_response script[EVAL_SCRIPT_MAX];
            size_t                          script_n = 0u;
            if (rules_case) {
                script[0] = (struct spg_fake_response){
                    .n = strlen(rules_script_line), .text = rules_script_line};
                script_n = 1u;
            } else {
                script_n = split_script_lines(script_text.data, script_text.n,
                                              script, EVAL_SCRIPT_MAX);
            }
            for (size_t s = 0u; s < samples; s += 1u) {
                struct spg_agent_run_inputs cin = inputs;
                if (has_goal) {
                    cin.goal = case_goal;
                }
                if (has_machine) {
                    cin.machine = &case_machine;
                }
                if (has_machine_after) {
                    cin.machine_after   = &case_machine_after;
                    cin.refresh_machine = true;
                }
                if (has_case_profile) {
                    cin.profile      = &case_profile;
                    cin.pause_ledger = &case_pause_ledger;
                }
                if (has_goal_file) {
                    cin.machine_goal = &case_machine_goal;
                }
                cin.machine_ablate = o->ablate;
                if (has_fixture) {
                    if (!eval_sandbox_prepare(&sandbox_state, name, s, fixture,
                                              store)) {
                        last = (struct spg_eval_case_result){
                            .outcome = SPG_EVAL_FAIL_RUN_ERROR,
                            .status  = SPG_E_IO};
                        report->total += 1u;
                        runs_in_case += 1u;
                        continue;
                    }
                    cin.store = &sandbox_state.store;
                }
                struct spg_eval_case_result r = {};
                const enum spg_status       cs =
                    spg_eval_run_case(script, script_n, gate_marker, &cin,
                                      &rcfg, &ws, &expect, &r);
                if (cs != SPG_OK) {
                    free_file_buffer(&script_text);
                    rc = cs;
                    goto done;
                }
                last = r;
                eval_tally_ladder(report, case_idx, &r);
                if (has_diagnosis) {
                    const struct spg_agent_loop_result synth = {
                        .steps_taken = r.steps_taken,
                        .termination = r.termination};
                    if (!eval_tally_diagnosis(
                            report, case_idx, expected_diag, model_output,
                            has_machine ? &case_machine : nullptr, &synth) &&
                        r.outcome == SPG_EVAL_PASS) {
                        r.outcome    = SPG_EVAL_FAIL_OBSERVATION;
                        last.outcome = SPG_EVAL_FAIL_OBSERVATION;
                    }
                }
                /* Phase 9: the goal decides, not the model. A run that
                 * emitted `finish` on a machine that still violates its
                 * constraints has not succeeded — and the objective check has
                 * to be able to OVERRULE a pass, or it is decoration. */
                if (has_goal_file) {
                    const struct spg_machine_state *observed =
                        has_machine_after ? &case_machine_after
                        : has_machine     ? &case_machine
                                          : nullptr;
                    struct spg_goal_evaluation ge = {};
                    if (observed != nullptr &&
                        spg_machine_goal_evaluate(&case_machine_goal, observed,
                                                  r.actions_executed,
                                                  &ge) == SPG_OK) {
                        report->goal_verdicts[case_idx] = ge.verdict;
                        if (ge.verdict != SPG_GOAL_SATISFIED &&
                            r.outcome == SPG_EVAL_PASS) {
                            r.outcome    = SPG_EVAL_FAIL_OBSERVATION;
                            last.outcome = SPG_EVAL_FAIL_OBSERVATION;
                        }
                    }
                }
                if (r.outcome == SPG_EVAL_PASS) {
                    passed_in_case += 1u;
                    report->passed += 1u;
                }
                report->total += 1u;
                runs_in_case += 1u;
            }
            free_file_buffer(&script_text);
        }

        report->case_latency_ms[case_idx] = bench_now_ms() - case_start;
        report->results[case_idx]         = last;
        report->runs[case_idx]            = runs_in_case;
        report->case_passed[case_idx]     = passed_in_case;
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

/* Print a model-supplied string as a JSON value.
 *
 * The reason is verbatim model output — the one field in this report that an
 * outside party writes. Emitting it raw put a newline inside a JSONL string
 * and truncated the line, and the tooling downstream read the unparseable line
 * as "this model produced no scoreable run". A measurement bug that looks like
 * a model result is the worst kind, so the escaping lives at the boundary
 * where the text becomes JSON. */
static void print_json_string(const char *text) {
    putchar('"');
    for (size_t i = 0u; text != nullptr && text[i] != '\0'; i += 1u) {
        const unsigned char c = (unsigned char)text[i];
        switch (c) {
        case '"':
            fputs("\\\"", stdout);
            break;
        case '\\':
            fputs("\\\\", stdout);
            break;
        case '\n':
            fputs("\\n", stdout);
            break;
        case '\r':
            fputs("\\r", stdout);
            break;
        case '\t':
            fputs("\\t", stdout);
            break;
        default:
            if (c < 0x20u) {
                printf("\\u%04x", c);
            } else {
                putchar((int)c);
            }
            break;
        }
    }
    putchar('"');
}

static void eval_print_report(const char                   *suite_path,
                              const struct eval_run_report *report) {
    for (size_t i = 0u; i < report->ncases; i += 1u) {
        const struct spg_eval_case_result *r = &report->results[i];
        printf("{\"name\":\"%s\",\"outcome\":\"%s\",\"termination\":\"%s\","
               "\"steps\":%zu,\"actions\":%zu,\"goal\":\"%s\",\"repairs\":%zu",
               report->names[i], spg_eval_outcome_to_string(r->outcome),
               spg_agent_loop_termination_to_string(r->termination),
               r->steps_taken, r->actions_executed,
               spg_goal_verdict_to_string(report->goal_verdicts[i]),
               r->repairs_used);
        if (report->report_timing) {
            printf(",\"latency_ms\":%llu",
                   (unsigned long long)report->case_latency_ms[i]);
        }
        /* With --samples N>1, aggregate k-of-N; N==1 stays byte-identical. */
        if (report->runs[i] > 1u) {
            printf(",\"runs\":%zu,\"passed\":%zu", report->runs[i],
                   report->case_passed[i]);
        }
        /* #53: the ladder, appended so existing consumers keep matching. */
        printf(",\"parsed\":%zu,\"gated\":%zu", report->case_parsed[i],
               report->case_gated[i]);
        /* #64: only for diagnosis cases, so every other suite's output stays
         * byte-identical to what its consumers already parse. */
        if (report->case_expected[i][0] != '\0') {
            printf(",\"expected\":\"%s\",\"emitted\":",
                   report->case_expected[i]);
            print_json_string(report->case_emitted[i]);
            printf(",\"correct\":%zu,\"hallucinated\":%zu"
                   ",\"action_proposed\":%zu,\"context_bytes\":%zu"
                   ",\"heldout\":%s,\"reason\":",
                   report->case_diag_ok[i], report->case_halluc[i],
                   report->case_action[i], report->case_ctx_bytes[i],
                   report->case_heldout[i] ? "true" : "false");
            print_json_string(report->case_reason[i]);
        }
        printf("}\n");
    }
    printf("{\"suite\":\"%s\",\"total\":%zu,\"passed\":%zu"
           ",\"parsed\":%zu,\"gated\":%zu",
           suite_path, report->total, report->passed, report->parsed,
           report->gated);
    if (report->report_timing) {
        printf(",\"latency_ms\":%llu,\"peak_rss_kb\":%llu",
               (unsigned long long)report->suite_latency_ms,
               (unsigned long long)report->peak_rss_kb);
    }
    if (report->diagnosis_suite) {
        size_t known_runs = 0u, known_ok = 0u, held_runs = 0u, held_ok = 0u;
        size_t halluc = 0u, actions = 0u;
        for (size_t i = 0u; i < report->ncases; i += 1u) {
            if (report->case_expected[i][0] == '\0') {
                continue;
            }
            /* Known and held-out are reported apart, never averaged: a suite
             * that hides the held-out split can look strong while having
             * learnt only the cases it was shown. */
            if (report->case_heldout[i]) {
                held_runs += report->runs[i];
                held_ok += report->case_diag_ok[i];
            } else {
                known_runs += report->runs[i];
                known_ok += report->case_diag_ok[i];
            }
            halluc += report->case_halluc[i];
            actions += report->case_action[i];
        }
        printf(",\"diagnosis\":{\"known\":%zu,\"known_runs\":%zu"
               ",\"heldout\":%zu,\"heldout_runs\":%zu"
               ",\"hallucinated\":%zu,\"action_proposed\":%zu}",
               known_ok, known_runs, held_ok, held_runs, halluc, actions);
    }
    printf("}\n");
}

static int eval_command(int argc, char **argv) {
    const char *suite_path    = nullptr;
    const char *remote_url    = nullptr;
    const char *remote_model  = nullptr;
    size_t      samples       = 1u;
    bool        constrained   = false;
    float       temperature   = 0.0f;
    bool        report_timing = false;
    /* Phase 11 (#71): withhold parts of the snapshot to find out what the
     * model actually uses. One mask applied at render time — no variant of the
     * renderer per experiment. */
    uint32_t ablate = SPG_ABLATE_NONE;
    /* #54: how to speak to this model. Precedence is CLI > profile file >
     * auto-detect, so a flag always beats a file and a file always beats the
     * guess. */
    const char *profile_model_path = nullptr;
    for (int i = 2; i < argc; i += 1) {
        if (strcmp(argv[i], "--model-profile") == 0 && i + 1 < argc) {
            profile_model_path = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--ablate") == 0 && i + 1 < argc) {
            const char *spec = argv[++i];
            const struct {
                const char *name;
                uint32_t    bit;
            } names[] = {
                {"role", SPG_ABLATE_ROLE},
                {"temperature", SPG_ABLATE_TEMPERATURE},
                {"frequency", SPG_ABLATE_FREQUENCY},
                {"memory", SPG_ABLATE_MEMORY},
                {"processes", SPG_ABLATE_PROCESSES},
                {"load", SPG_ABLATE_LOAD},
            };
            for (size_t k = 0u; k < sizeof names / sizeof names[0]; k += 1u) {
                if (strstr(spec, names[k].name) != nullptr) {
                    ablate |= names[k].bit;
                }
            }
            if (ablate == SPG_ABLATE_NONE) {
                /* An unrecognised spec would quietly run the full context and
                 * be recorded as an ablation — the silent kind of wrong number
                 * this phase is meant to avoid producing. */
                fprintf(stderr, "eval: --ablate %s matched nothing\n", spec);
                return 2;
            }
            continue;
        }
        if (strcmp(argv[i], "--timing") == 0) {
            /* Adds wall-clock and peak RSS to the report. Off by default so
             * two identical runs stay byte-identical — the property the
             * fixture test relies on, and the one that makes a report worth
             * diffing at all. */
            report_timing = true;
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
                fprintf(stderr, "eval: invalid --samples value\n");
                return 2;
            }
            continue;
        }
        if (strcmp(argv[i], "--constrained") == 0) {
            constrained = true;
            continue;
        }
        if (strcmp(argv[i], "--temperature") == 0 && i + 1 < argc) {
            if (!parse_temperature(argv[++i], &temperature)) {
                fprintf(stderr, "eval: invalid --temperature value\n");
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
                "[--remote-model <name>] [--samples <N>] [--constrained] "
                "[--temperature <t>]\n",
                argv[0]);
        return 2;
    }
    static struct eval_run_report report;
    const uint64_t                suite_start_ms = bench_now_ms();
    const struct eval_run_opts    opts = {.remote_url   = remote_url,
                                          .remote_model = remote_model,
                                          .samples      = samples,
                                          .constrained  = constrained,
                                          .temperature  = temperature,
                                          .ablate       = ablate,
                                          .profile_model_path =
                                              profile_model_path};
    const enum spg_status         status =
        eval_run_suite(suite_path, nullptr, &opts, &report);
    if (status != SPG_OK) {
        fprintf(stderr, "eval: suite run failed: %s\n",
                spg_status_to_string(status));
        return 1;
    }
    report.suite_latency_ms = bench_now_ms() - suite_start_ms;
    report.peak_rss_kb      = bench_peak_rss_kb();
    report.report_timing    = report_timing;
    eval_print_report(suite_path, &report);
    return (report.total > 0u && report.passed == report.total) ? 0 : 1;
}

/* ---- P5 (Weg 2): live guard re-run gate --------------------------------- *
 * A guard is a real run config that finished and passed its (expect). At mint
 * time it is re-run live so the real model reacts to the candidate lesson in
 * the mind-palace; a guard that passed without the lesson and fails with it
 * vetoes it. The guard is run by synthesising a one-case suite over its config
 * and reusing eval_run_suite — no new model orchestration. Without a real
 * model the run fails at baseline, which spg_guard_survives treats as no
 * signal, so guards degrade to inert rather than veto spuriously. */
struct guard_ctx {
    struct spg_mem_store       *store;
    const struct eval_run_opts *opts;
    const struct spg_lesson    *lesson;
};

/* Escape a string into an s-expr double-quoted literal at *w (bounded). */
static void sexpr_escape(char *dst, size_t cap, size_t *w, const char *s) {
    for (const char *p = s; *p != '\0' && *w + 2u < cap; p++) {
        if (*p == '"' || *p == '\\') {
            dst[(*w)++] = '\\';
        }
        dst[(*w)++] = *p;
    }
}

static bool guard_run(void *vctx, const char *config_path, bool with_lesson) {
    struct guard_ctx *ctx = (struct guard_ctx *)vctx;

    /* toggle the candidate lesson so the real model sees it (trial) or not
     * (baseline); cheap file ops, isolating the lesson's effect on the guard */
    if (with_lesson) {
        (void)spg_mem_save(ctx->store, ctx->lesson->slug,
                           ctx->lesson->description, ctx->lesson->body);
    } else {
        (void)spg_mem_delete(ctx->store, ctx->lesson->slug);
    }

    /* the guard's own (expect) criterion (P1) is what judges it */
    struct file_buffer    cfg_text = {};
    struct spg_run_config cfg      = {};
    if (load_run_file(config_path, &cfg_text, &cfg) != SPG_OK ||
        !cfg.has_expect) {
        free_file_buffer(&cfg_text);
        return false; /* no criterion -> cannot judge -> no signal (baseline
                         fail) */
    }
    char   expect[AGENT_OBS_BYTES];
    size_t en = cfg.expect_observation.length < sizeof expect
                    ? cfg.expect_observation.length
                    : sizeof expect - 1u;
    memcpy(expect, cfg_text.data + cfg.expect_observation.offset, en);
    expect[en] = '\0';
    free_file_buffer(&cfg_text);

    /* synthesise a one-case suite over the guard config; (model "geist") uses
     * the real model from that config, with the mind-palace injected */
    char   suite[1024];
    size_t w = (size_t)snprintf(suite, sizeof suite, "(eval_suite (config \"");
    sexpr_escape(suite, sizeof suite, &w, config_path);
    w += (size_t)snprintf(suite + w, sizeof suite - w,
                          "\") (case (name \"guard\") (model \"geist\") "
                          "(allow_exec) (max_steps 8) (expect (observation \"");
    sexpr_escape(suite, sizeof suite, &w, expect);
    w += (size_t)snprintf(suite + w, sizeof suite - w, "\")))) ");
    if (w >= sizeof suite) {
        return false;
    }

    char tmpl[] = "/tmp/spg_guard_XXXXXX";
    int  fd     = mkstemp(tmpl);
    if (fd < 0) {
        return false;
    }
    const bool wrote = write(fd, suite, w) == (ssize_t)w;
    (void)close(fd);
    bool passed = false;
    if (wrote) {
        struct eval_run_report report = {};
        if (eval_run_suite(tmpl, ctx->store, ctx->opts, &report) == SPG_OK) {
            passed = report.total > 0u && report.passed == report.total;
        }
    }
    (void)unlink(tmpl);
    return passed;
}

/* Distil a success SKILL from a passing run's journal (docs/LEARNING.md /
 * geistshell#26). OFFLINE by design: the caller invokes this only for a run
 * that met its criterion — the live agent never self-mints skills.
 * Reconstructs the trajectory (P3), computes the capability shape (P4) and the
 * ordered-kind procedure, distils a skill-<shape> memory, and saves it. The
 * saved skill is injected on later runs via the mind-palace index the agent
 * already renders into context. */
static int distill_command(int argc, char **argv) {
    const char *journal    = nullptr;
    const char *memory_dir = nullptr;
    for (int i = 2; i < argc; i += 1) {
        if (strcmp(argv[i], "--memory-dir") == 0 && i + 1 < argc) {
            memory_dir = argv[++i];
            continue;
        }
        if (argv[i][0] != '-' && journal == nullptr) {
            journal = argv[i];
            continue;
        }
        fprintf(stderr, "distill: unexpected argument: %s\n", argv[i]);
        return 2;
    }
    if (journal == nullptr) {
        fprintf(stderr, "usage: %s distill <journal.sgj> [--memory-dir <d>]\n",
                argv[0]);
        return 2;
    }

    static struct spg_fake_response responses[AGENT_MAX_SCRIPT];
    static char                     text[CLI_MODEL_OUTPUT_BYTES];
    size_t                          n = 0u;
    if (spg_eval_script_from_journal(journal, AGENT_MAX_SCRIPT, responses,
                                     sizeof text, text, &n) != SPG_OK ||
        n == 0u) {
        fprintf(stderr, "distill: no trajectory in %s\n", journal);
        return 1;
    }

    char   shape[256];
    size_t shape_n = 0u;
    if (spg_shape_from_script(responses, n, sizeof shape, shape, &shape_n) !=
            SPG_OK ||
        shape_n == 0u) {
        fprintf(stderr, "distill: could not derive a shape\n");
        return 1;
    }

    /* ordered-kind procedure summary, e.g. "local_shell -> finish" */
    char   procedure[256];
    size_t pw = 0u;
    for (size_t i = 0u; i < n; i += 1u) {
        struct spg_sexpr_token          toks[256];
        struct spg_sexpr_node           nodes[256];
        struct spg_recommendation       rec = {};
        struct spg_recommendation_error err = {};
        if (spg_recommendation_parse(responses[i].n, responses[i].text, 256u,
                                     toks, 256u, nodes, &rec, &err) != SPG_OK) {
            continue;
        }
        const char *kind = spg_action_kind_to_string(rec.action_kind);
        pw += (size_t)snprintf(procedure + pw, sizeof procedure - pw, "%s%s",
                               pw > 0u ? " -> " : "", kind);
        if (pw >= sizeof procedure) {
            break;
        }
    }

    struct spg_lesson skill = {};
    if (!spg_reflect_skill(shape, procedure, &skill)) {
        fprintf(stderr, "distill: could not distil a skill\n");
        return 1;
    }

    struct spg_mem_store store;
    if (spg_mem_store_open(&store, spg_mem_resolve_dir(memory_dir)) != SPG_OK) {
        fprintf(stderr, "distill: cannot open memory dir\n");
        return 1;
    }
    if (spg_mem_save(&store, skill.slug, skill.description, skill.body) !=
        SPG_OK) {
        fprintf(stderr, "distill: cannot save %s\n", skill.slug);
        return 1;
    }
    printf("{\"skill\":\"%s\",\"shape\":\"%s\",\"procedure\":\"%s\"}\n",
           skill.slug, shape, procedure);
    return 0;
}

/* Modification time in whole seconds; false when the file is unreadable. */
static bool file_mtime(const char *path, time_t *out) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    *out = st.st_mtime;
    return true;
}

/* The k-th counter of a recurrence tally, in the order of audit_slugs below —
 * C has no pointer-to-member, and two counters do not earn a callback. */
static size_t recurrence_slot(const struct spg_recurrence *r, const size_t k) {
    return k == 0u ? r->rejected : r->denied;
}

static const char *const audit_slugs[] = {"lesson-rejected", "lesson-denied"};

/* Longitudinal benefit audit (docs/LEARNING.md P7): a kept lesson earns its
 * keep only if its failure slug stops recurring in runs that happened AFTER it
 * was minted. Summing every journal into one bucket cannot show that — the
 * lesson exists *because* of a failure, so the pre-mint runs guarantee a
 * non-zero count and the verdict would read "review" forever. One journal file
 * is one run (the writer truncates on open), so the file count is the run count
 * and the file's mtime is when that run finished; a lesson's mtime is when it
 * was saved. Split the journals at the lesson's mtime, count hits per run on
 * each side, and compare the two rates. Model-free: journals and mtimes only.
 * ponytail: mtime at one-second granularity, and a run that STARTED before the
 * mint but finished after it lands on the "after" side. Both only matter if
 * runs and mints interleave inside a second; the nightly loop this is for is
 * days apart. Stamping the mint time into the journal's first record would be
 * exact — wire that when a run can outlive a mint. */
static int audit_command(int argc, char **argv) {
    const char *memory_dir = nullptr;
    const char *journals[EVAL_MAX_CASES];
    size_t      njournals = 0u;
    for (int i = 2; i < argc; i += 1) {
        if (strcmp(argv[i], "--memory-dir") == 0 && i + 1 < argc) {
            memory_dir = argv[++i];
            continue;
        }
        if (argv[i][0] != '-' && njournals < EVAL_MAX_CASES) {
            journals[njournals++] = argv[i];
            continue;
        }
        fprintf(stderr, "audit: unexpected argument: %s\n", argv[i]);
        return 2;
    }
    if (njournals == 0u) {
        fprintf(stderr, "usage: %s audit <journal.sgj>... [--memory-dir <d>]\n",
                argv[0]);
        return 2;
    }

    /* Per journal, so the tallies can be re-split at any lesson's mint time
     * without re-reading the files. */
    struct spg_recurrence per[EVAL_MAX_CASES] = {};
    time_t                finished[EVAL_MAX_CASES] = {};
    struct spg_recurrence total = {};
    for (size_t i = 0u; i < njournals; i += 1u) {
        if (spg_journal_recurrence(journals[i], &per[i]) != SPG_OK) {
            fprintf(stderr, "audit: cannot read journal %s\n", journals[i]);
            return 1;
        }
        if (!file_mtime(journals[i], &finished[i])) {
            finished[i] = 0; /* unreadable mtime: counts as an early run */
        }
        total.rejected += per[i].rejected;
        total.denied += per[i].denied;
    }
    printf("{\"journals\":%zu,\"lesson-rejected\":%zu,\"lesson-denied\":%zu}\n",
           njournals, total.rejected, total.denied);

    /* Cross-reference kept lessons: for each one, the failure rate before it
     * existed against the rate since. */
    if (memory_dir != nullptr) {
        struct spg_mem_store store;
        if (spg_mem_store_open(&store, spg_mem_resolve_dir(memory_dir)) !=
            SPG_OK) {
            fprintf(stderr, "audit: cannot open memory dir\n");
            return 1;
        }
        char probe[SPG_MEM_DESC_MAX + 1u];
        for (size_t k = 0u; k < sizeof audit_slugs / sizeof audit_slugs[0];
             k += 1u) {
            if (spg_mem_directive(&store, audit_slugs[k], 0u, sizeof probe,
                                  probe) == 0u) {
                continue; /* no such lesson kept */
            }
            char path[SPG_MEM_PATH_MAX];
            (void)snprintf(path, sizeof path, "%s/%s.md", store.dir,
                           audit_slugs[k]);
            time_t minted = 0;
            if (!file_mtime(path, &minted)) {
                continue;
            }

            size_t before_runs = 0u, after_runs = 0u, before = 0u, after = 0u;
            for (size_t i = 0u; i < njournals; i += 1u) {
                if (finished[i] > minted) {
                    after_runs += 1u;
                    after += recurrence_slot(&per[i], k);
                } else {
                    before_runs += 1u;
                    before += recurrence_slot(&per[i], k);
                }
            }
            /* Rates compared by cross-multiplication, so no floats and no
             * divide-by-zero. "pending" is the honest verdict while no run has
             * yet had the chance to benefit. */
            const char *verdict =
                after_runs == 0u ? "pending"
                : after == 0u    ? "kept"
                : after * before_runs < before * after_runs ? "improving"
                                                            : "review";
            printf("{\"lesson\":\"%s\",\"before\":{\"runs\":%zu,\"hits\":%zu},"
                   "\"after\":{\"runs\":%zu,\"hits\":%zu},\"verdict\":\"%s\"}\n",
                   audit_slugs[k], before_runs, before, after_runs, after,
                   verdict);
        }
    }
    return 0;
}

/* Self-improvement: run the suite, distill a lesson for each failing case,
 * persist each tentatively into the mind-palace, re-run, and keep it only if
 * the pass count did not drop (else revert). Emits a JSONL report. */
static int improve_command(int argc, char **argv) {
    const char           *suite_path    = nullptr;
    const char           *memory_dir    = nullptr;
    const char           *remote_url    = nullptr;
    const char           *remote_model  = nullptr;
    const char           *validate_path = nullptr;
    size_t                samples       = 1u;
    bool                  constrained   = false;
    float                 temperature   = 0.0f;
    struct spg_guard_ring guards;
    spg_guard_ring_init(&guards);
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
        /* #51: the gate must measure the decoder the agent actually runs — a
         * lesson kept under free decode says nothing about a constrained one.
         */
        if (strcmp(argv[i], "--constrained") == 0) {
            constrained = true;
            continue;
        }
        if (strcmp(argv[i], "--temperature") == 0 && i + 1 < argc) {
            if (!parse_temperature(argv[++i], &temperature)) {
                fprintf(stderr, "improve: invalid --temperature value\n");
                return 2;
            }
            continue;
        }
        if (strcmp(argv[i], "--guard") == 0 && i + 1 < argc) {
            /* a real run config re-run live to gate lessons (P5, Weg 2);
             * repeatable, deduped by shape=path in the ring */
            spg_guard_ring_record(&guards, argv[i + 1], argv[i + 1]);
            i += 1;
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
        fprintf(
            stderr,
            "usage: %s improve <suite.spg> [--validate <holdout.spg>] "
            "[--guard <run.spg>]... "
            "[--memory-dir <d>] [--remote-url <url>] [--remote-model <name>] "
            "[--samples <N>] [--constrained] [--temperature <t>]\n",
            argv[0]);
        return 2;
    }

    struct spg_mem_store store;
    if (spg_mem_store_open(&store, spg_mem_resolve_dir(memory_dir)) != SPG_OK) {
        fprintf(stderr, "improve: cannot open memory dir %s\n",
                spg_mem_resolve_dir(memory_dir));
        return 1;
    }

    const struct eval_run_opts    opts = {.remote_url   = remote_url,
                                          .remote_model = remote_model,
                                          .samples      = samples,
                                          .constrained  = constrained,
                                          .temperature  = temperature};
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
     * cases it was not derived from, not merely because it fit the suite it
     * came from. Without --validate the gate falls back to the train suite (the
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
        /* Suite gate first; then, if it would keep, the live guard gate
         * (P5, Weg 2): re-run every guard with vs without the lesson and veto
         * if any that passed now regresses. A guard vetoes only a lesson the
         * suite already accepts, so it can only make the gate stricter. */
        bool accepted     = spg_improve_accept(cur_passed, trial.passed);
        bool guard_vetoed = false;
        if (accepted) {
            struct guard_ctx gctx = {
                .store = &store, .opts = &opts, .lesson = lesson};
            if (!spg_guard_ring_gate(&guards, guard_run, &gctx)) {
                accepted     = false;
                guard_vetoed = true;
            }
            /* the gate's guard runs toggled the lesson; leave it saved so the
             * commit below decides keep/revert from a known state */
            (void)spg_mem_save(&store, lesson->slug, lesson->description,
                               lesson->body);
        }
        (void)guard_vetoed;
        bool was_kept = false;
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
        printf(
            "{\"suite\":\"%s\",\"validate\":\"%s\",\"held_out_baseline\":%zu,"
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
        fprintf(
            stderr,
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

/* Verify a journal's keyed seal; prints signed=true/false, non-zero on
 * mismatch. */
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

    if (strcmp(argv[1], "distill") == 0) {
        return distill_command(argc, argv);
    }

    if (strcmp(argv[1], "audit") == 0) {
        return audit_command(argc, argv);
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
        if (argc < 3 || argc > 4 ||
            (argc == 4 && strcmp(argv[3], "--payloads") != 0)) {
            print_replay_usage(argv[0]);
            return 2;
        }
        return replay_command(argv[2], argc == 4);
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

    if (strcmp(argv[1], "device") == 0) {
        return device_command(argc, argv);
    }

    fprintf(stderr, "%s: unknown command\n", argv[1]);
    print_usage(argv[0]);
    return 2;
}
