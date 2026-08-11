/* Reads a (machine-state ...) block back into a snapshot — the inverse of
 * spg_machine_state_render.
 *
 * Why this exists: a diagnosis scenario IS a machine state, and an eval case
 * must be reproducible on a laptop with no Pi attached. Sampling the host would
 * make every case depend on what happens to run on the CI machine. A round-trip
 * test (render -> parse -> render, byte-identical) keeps the two halves of the
 * format from drifting apart. */

#include "geistshell/machine_fixture.h"

#include <string.h>

static bool span_is(const size_t input_n, const char input[],
                    const struct spg_text_span span, const char *text) {
    return spg_sexpr_span_eq_cstr(input_n, input, span, text);
}

/* The value node of (name value) among node's children, or INVALID. */
static uint32_t field_value(const size_t input_n, const char input[],
                            const struct spg_sexpr_node nodes[static 1],
                            const uint32_t parent, const char *name) {
    uint32_t field = spg_sexpr_first_child(nodes, parent);
    while (field != SPG_SEXPR_INVALID_INDEX) {
        if (nodes[field].kind == SPG_SEXPR_NODE_LIST) {
            const uint32_t head = spg_sexpr_first_child(nodes, field);
            if (head != SPG_SEXPR_INVALID_INDEX &&
                span_is(input_n, input, nodes[head].span, name)) {
                return spg_sexpr_second_child(nodes, field);
            }
        }
        field = nodes[field].next_sibling;
    }
    return SPG_SEXPR_INVALID_INDEX;
}

/* An absent field and an explicit `unknown` mean the same thing: no value. The
 * sentinel is never 0 — a consumer must not read a dead sensor as an idle
 * machine. */
static enum spg_status read_u64(const size_t input_n, const char input[],
                                const struct spg_sexpr_node nodes[static 1],
                                const uint32_t parent, const char *name,
                                uint64_t *out) {
    const uint32_t value = field_value(input_n, input, nodes, parent, name);
    if (value == SPG_SEXPR_INVALID_INDEX) {
        *out = SPG_MACHINE_UNKNOWN;
        return SPG_OK;
    }
    if (span_is(input_n, input, nodes[value].span, "unknown")) {
        *out = SPG_MACHINE_UNKNOWN;
        return SPG_OK;
    }
    return spg_sexpr_parse_uint64_span(input_n, input, nodes[value].span, out);
}

static enum spg_status read_i64(const size_t input_n, const char input[],
                                const struct spg_sexpr_node nodes[static 1],
                                const uint32_t parent, const char *name,
                                int64_t *out) {
    const uint32_t value = field_value(input_n, input, nodes, parent, name);
    if (value == SPG_SEXPR_INVALID_INDEX ||
        span_is(input_n, input, nodes[value].span, "unknown")) {
        *out = SPG_MACHINE_UNKNOWN_S;
        return SPG_OK;
    }
    struct spg_text_span span   = nodes[value].span;
    bool                 negate = false;
    if (span.length > 0u && span.offset < input_n &&
        input[span.offset] == '-') {
        negate = true;
        span.offset += 1u;
        span.length -= 1u;
    }
    uint64_t              magnitude = 0u;
    const enum spg_status status =
        spg_sexpr_parse_uint64_span(input_n, input, span, &magnitude);
    if (status != SPG_OK) {
        return status;
    }
    if (magnitude > (uint64_t)INT64_MAX) {
        return SPG_E_OVERFLOW;
    }
    *out = negate ? -(int64_t)magnitude : (int64_t)magnitude;
    return SPG_OK;
}

static enum spg_status read_role(const size_t input_n, const char input[],
                                 const struct spg_sexpr_node nodes[static 1],
                                 const uint32_t              parent,
                                 enum spg_process_role      *out) {
    const uint32_t value = field_value(input_n, input, nodes, parent, "role");
    if (value == SPG_SEXPR_INVALID_INDEX ||
        span_is(input_n, input, nodes[value].span, "unknown")) {
        *out = SPG_PROCESS_ROLE_UNKNOWN;
        return SPG_OK;
    }
    if (span_is(input_n, input, nodes[value].span, "critical")) {
        *out = SPG_PROCESS_ROLE_CRITICAL;
        return SPG_OK;
    }
    if (span_is(input_n, input, nodes[value].span, "batch")) {
        *out = SPG_PROCESS_ROLE_BATCH;
        return SPG_OK;
    }
    /* A misspelt role must not quietly become `unknown` and strip a critical
     * process of its protection — same rule as the profile parser. */
    return SPG_E_SCHEMA;
}

static enum spg_throttle_state
read_throttle(const size_t input_n, const char input[],
              const struct spg_sexpr_node nodes[static 1],
              const uint32_t              parent) {
    const uint32_t value =
        field_value(input_n, input, nodes, parent, "throttle");
    if (value == SPG_SEXPR_INVALID_INDEX) {
        return SPG_THROTTLE_UNKNOWN;
    }
    if (span_is(input_n, input, nodes[value].span, "none")) {
        return SPG_THROTTLE_NONE;
    }
    if (span_is(input_n, input, nodes[value].span, "active")) {
        return SPG_THROTTLE_ACTIVE;
    }
    if (span_is(input_n, input, nodes[value].span, "past")) {
        return SPG_THROTTLE_PAST;
    }
    return SPG_THROTTLE_UNKNOWN;
}

static enum spg_status read_process(const size_t input_n, const char input[],
                                    const struct spg_sexpr_node nodes[static 1],
                                    const uint32_t              entry,
                                    struct spg_process_sample  *out) {
    *out = (struct spg_process_sample){
        .cpu_bp        = SPG_MACHINE_UNKNOWN,
        .rss_bytes     = SPG_MACHINE_UNKNOWN,
        .profile_index = SPG_PROCESS_NO_PROFILE,
    };
    const uint32_t       id   = field_value(input_n, input, nodes, entry, "id");
    struct spg_text_span span = {};
    if (id == SPG_SEXPR_INVALID_INDEX ||
        !spg_sexpr_string_payload_span(&nodes[id], &span) ||
        !spg_sexpr_span_valid(input_n, span) || span.length == 0u ||
        span.length + 1u > SPG_PROCESS_ID_CAP) {
        return SPG_E_SCHEMA;
    }
    memcpy(out->profile_id, input + span.offset, span.length);
    out->profile_id[span.length] = '\0';
    /* A fixture describes what the context shows, so the id doubles as the
     * name; the role decides whether it counts as managed. */
    const size_t name_n = span.length + 1u > SPG_PROCESS_NAME_CAP
                              ? SPG_PROCESS_NAME_CAP - 1u
                              : span.length;
    memcpy(out->name, input + span.offset, name_n);
    out->name[name_n] = '\0';

    enum spg_status status =
        read_role(input_n, input, nodes, entry, &out->role);
    if (status != SPG_OK) {
        return status;
    }
    if (out->role != SPG_PROCESS_ROLE_UNKNOWN) {
        out->profile_index = 0u; /* managed: ranks ahead of the rest */
        out->may_pause     = out->role == SPG_PROCESS_ROLE_BATCH;
        out->may_stop      = out->role == SPG_PROCESS_ROLE_BATCH;
    } else {
        out->profile_id[0] = '\0';
    }
    status = read_u64(input_n, input, nodes, entry, "cpu-bp", &out->cpu_bp);
    if (status != SPG_OK) {
        return status;
    }
    return read_u64(input_n, input, nodes, entry, "rss-bytes", &out->rss_bytes);
}

enum spg_status
spg_machine_state_parse(const size_t input_n, const char input[],
                        const size_t              token_capacity,
                        struct spg_sexpr_token    tokens[static token_capacity],
                        const size_t              node_capacity,
                        struct spg_sexpr_node     nodes[static node_capacity],
                        struct spg_machine_state *out) {
    if (out == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out = (struct spg_machine_state){};

    size_t                 token_count = 0u;
    size_t                 node_count  = 0u;
    struct spg_sexpr_error error       = {};
    enum spg_status        status      = spg_sexpr_parse_text(
        input_n, input, token_capacity, tokens, node_capacity, nodes,
        &token_count, &node_count, &error);
    if (status != SPG_OK) {
        return status;
    }
    if (node_count == 0u) {
        return SPG_E_FORMAT;
    }
    const uint32_t root = 0u;
    const uint32_t head = spg_sexpr_first_child(nodes, root);
    if (nodes[root].kind != SPG_SEXPR_NODE_LIST ||
        head == SPG_SEXPR_INVALID_INDEX ||
        !span_is(input_n, input, nodes[head].span, "machine-state")) {
        return SPG_E_SCHEMA;
    }

    struct {
        const char *name;
        uint64_t   *slot;
    } const u64_fields[] = {
        {"cpu-load-bp", &out->cpu_utilisation_bp},
        {"load-1-cbp", &out->load_1_cbp},
        {"memory-total-bytes", &out->memory.total_bytes},
        {"memory-used-bytes", &out->memory.used_bytes},
        {"swap-used-bytes", &out->memory.swap_used_bytes},
        {"cpu-freq-khz", &out->cpu_freq_khz},
        {"process-count", &out->process_count},
    };
    for (size_t i = 0u; i < sizeof u64_fields / sizeof u64_fields[0]; i += 1u) {
        status = read_u64(input_n, input, nodes, root, u64_fields[i].name,
                          u64_fields[i].slot);
        if (status != SPG_OK) {
            return status;
        }
    }
    status = read_i64(input_n, input, nodes, root, "temperature-mc",
                      &out->temperature_mc);
    if (status != SPG_OK) {
        return status;
    }
    out->throttle = read_throttle(input_n, input, nodes, root);

    uint64_t dropped = 0u;
    status =
        read_u64(input_n, input, nodes, root, "processes-dropped", &dropped);
    if (status != SPG_OK) {
        return status;
    }
    out->processes_truncated =
        field_value(input_n, input, nodes, root, "processes-dropped") !=
        SPG_SEXPR_INVALID_INDEX;

    for (uint32_t entry                          = nodes[head].next_sibling;
         entry != SPG_SEXPR_INVALID_INDEX; entry = nodes[entry].next_sibling) {
        if (nodes[entry].kind != SPG_SEXPR_NODE_LIST) {
            continue;
        }
        const uint32_t entry_head = spg_sexpr_first_child(nodes, entry);
        if (entry_head == SPG_SEXPR_INVALID_INDEX ||
            !span_is(input_n, input, nodes[entry_head].span, "process")) {
            continue;
        }
        if (out->n_processes >= SPG_MACHINE_MAX_PROCESSES) {
            return SPG_E_LIMIT;
        }
        status = read_process(input_n, input, nodes, entry,
                              &out->processes[out->n_processes]);
        if (status != SPG_OK) {
            return status;
        }
        out->n_processes += 1u;
    }
    return SPG_OK;
}
