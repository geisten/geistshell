/* The channel table and the one transport under it: a program.
 *
 * Split in three parts, in order of how testable they are: the table (pure),
 * the config form (pure), the exec (bounded by spg_cmd_executor_run).
 * Everything that decides anything lives in the first two — every refusal is
 * made before a fork, so all of it runs in tests where no program exists. */

/* getaddrinfo/freeaddrinfo and struct addrinfo are POSIX, and glibc hides them
 * unless a feature-test macro asks for them. `-std=c23` defines __STRICT_ANSI__,
 * so on Linux this file did not compile at all — while macOS and the BSDs
 * declare them by default and never complained. Found by the first CI run that
 * built this repo on Linux (#105); the same 200809L the other POSIX-using
 * sources in this tree already declare. */
#define _POSIX_C_SOURCE 200809L

#include "geistshell/device.h"

#include "geistshell/cmd_executor.h"
#include "geistshell/sexpr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Enough for a full (device ...) table of 32 channels; a config that needs
 * more than this is not a config, it is a program. Stack, not heap — the
 * no-allocation discipline of the rest of the codebase. */
#define DEVICE_TOKENS 1024u
#define DEVICE_NODES  512u

/* --- The table -------------------------------------------------------- */

void spg_device_init(struct spg_device *dev) {
    if (dev == nullptr) {
        return;
    }
    *dev = (struct spg_device){.n_channels = 0u};
}

enum spg_status
spg_device_add_channel(struct spg_device               *dev,
                       const struct spg_device_channel *channel) {
    if (dev == nullptr || channel == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    if (channel->name[0] == '\0' || channel->program[0] == '\0' ||
        channel->min > channel->max) {
        return SPG_E_INVALID_ARG;
    }
    if (channel->writable &&
        (channel->safe < channel->min || channel->safe > channel->max)) {
        /* A safe value the channel cannot accept is not a fallback — the
         * watchdog would fire and then be refused by the range check. */
        return SPG_E_INVALID_ARG;
    }
    if (spg_device_find(dev, channel->name) != nullptr) {
        /* Two channels under one name means a write could reach either
         * program depending on table order. Refuse the table instead. */
        return SPG_E_INVALID_ARG;
    }
    if (dev->n_channels >= SPG_DEVICE_MAX_CHANNELS) {
        return SPG_E_LIMIT;
    }
    dev->channels[dev->n_channels] = *channel;
    dev->n_channels += 1u;
    return SPG_OK;
}

const struct spg_device_channel *spg_device_find(const struct spg_device *dev,
                                                 const char *name) {
    if (dev == nullptr || name == nullptr) {
        return nullptr;
    }
    for (size_t i = 0u; i < dev->n_channels; i += 1u) {
        if (strcmp(dev->channels[i].name, name) == 0) {
            return &dev->channels[i];
        }
    }
    return nullptr;
}

/* --- The config form --------------------------------------------------- */

/* Parse a signed integer that spans a whole symbol node. */
static bool span_to_i64(const size_t input_n, const char input[],
                        const struct spg_text_span span, int64_t *out) {
    if (span.length == 0u || span.length >= 24u ||
        span.offset > input_n || span.length > input_n - span.offset) {
        return false;
    }
    char buffer[24];
    memcpy(buffer, input + span.offset, span.length);
    buffer[span.length] = '\0';
    char         *end   = nullptr;
    const int64_t value = (int64_t)strtoll(buffer, &end, 10);
    if (end != buffer + span.length) {
        return false;
    }
    *out = value;
    return true;
}

static bool span_to_text(const size_t input_n, const char input[],
                         const struct spg_text_span span, const size_t cap,
                         char out[]) {
    if (span.length == 0u || span.length + 1u > cap ||
        span.offset > input_n || span.length > input_n - span.offset) {
        return false;
    }
    memcpy(out, input + span.offset, span.length);
    out[span.length] = '\0';
    return true;
}

/* One (channel ...) node into a channel struct. (safe ...) present means
 * writable — see device.h for why that is one statement, not two. */
static enum spg_status
parse_channel_node(const size_t input_n, const char input[],
                   const struct spg_sexpr_node nodes[], const uint32_t channel,
                   struct spg_device_channel *out) {
    *out           = (struct spg_device_channel){};
    bool has_name  = false;
    bool has_prog  = false;
    bool has_range = false;

    const uint32_t head = spg_sexpr_first_child(nodes, channel);
    if (head == SPG_SEXPR_INVALID_INDEX ||
        nodes[head].kind != SPG_SEXPR_NODE_SYMBOL ||
        !spg_sexpr_span_eq_cstr(input_n, input, nodes[head].span, "channel")) {
        return SPG_E_SCHEMA;
    }
    for (uint32_t field = nodes[head].next_sibling;
         field != SPG_SEXPR_INVALID_INDEX; field = nodes[field].next_sibling) {
        if (nodes[field].kind != SPG_SEXPR_NODE_LIST) {
            return SPG_E_SCHEMA;
        }
        const uint32_t key   = spg_sexpr_first_child(nodes, field);
        const uint32_t value = spg_sexpr_second_child(nodes, field);
        if (key == SPG_SEXPR_INVALID_INDEX ||
            value == SPG_SEXPR_INVALID_INDEX ||
            nodes[key].kind != SPG_SEXPR_NODE_SYMBOL) {
            return SPG_E_SCHEMA;
        }
        if (spg_sexpr_span_eq_cstr(input_n, input, nodes[key].span, "name")) {
            struct spg_text_span payload = {};
            if (nodes[value].kind != SPG_SEXPR_NODE_STRING ||
                !spg_sexpr_string_payload_span(&nodes[value], &payload) ||
                !span_to_text(input_n, input, payload, SPG_DEVICE_NAME_CAP,
                              out->name)) {
                return SPG_E_SCHEMA;
            }
            has_name = true;
        } else if (spg_sexpr_span_eq_cstr(input_n, input, nodes[key].span,
                                          "program")) {
            struct spg_text_span payload = {};
            if (nodes[value].kind != SPG_SEXPR_NODE_STRING ||
                !spg_sexpr_string_payload_span(&nodes[value], &payload) ||
                !span_to_text(input_n, input, payload, SPG_DEVICE_PROGRAM_CAP,
                              out->program)) {
                return SPG_E_SCHEMA;
            }
            has_prog = true;
        } else if (spg_sexpr_span_eq_cstr(input_n, input, nodes[key].span,
                                          "range")) {
            const uint32_t hi = nodes[value].next_sibling;
            if (nodes[value].kind != SPG_SEXPR_NODE_SYMBOL ||
                hi == SPG_SEXPR_INVALID_INDEX ||
                nodes[hi].kind != SPG_SEXPR_NODE_SYMBOL ||
                !span_to_i64(input_n, input, nodes[value].span, &out->min) ||
                !span_to_i64(input_n, input, nodes[hi].span, &out->max)) {
                return SPG_E_SCHEMA;
            }
            has_range = true;
        } else if (spg_sexpr_span_eq_cstr(input_n, input, nodes[key].span,
                                          "safe")) {
            if (nodes[value].kind != SPG_SEXPR_NODE_SYMBOL ||
                !span_to_i64(input_n, input, nodes[value].span, &out->safe)) {
                return SPG_E_SCHEMA;
            }
            out->writable = true;
        } else if (spg_sexpr_span_eq_cstr(input_n, input, nodes[key].span,
                                          "network")) {
            /* #119: operator-declared transport need. Only the two symbols —
             * a typo here would silently change a trust decision. */
            if (nodes[value].kind != SPG_SEXPR_NODE_SYMBOL) {
                return SPG_E_SCHEMA;
            }
            if (spg_sexpr_span_eq_cstr(input_n, input, nodes[value].span,
                                       "true")) {
                out->network = true;
            } else if (spg_sexpr_span_eq_cstr(input_n, input,
                                              nodes[value].span, "false")) {
                out->network = false;
            } else {
                return SPG_E_SCHEMA;
            }
        } else {
            return SPG_E_SCHEMA; /* an unknown field is a misspelled field */
        }
    }
    if (!has_name || !has_prog || !has_range) {
        return SPG_E_SCHEMA;
    }
    return SPG_OK;
}

enum spg_status spg_device_parse_channel(const size_t text_n,
                                         const char   text[],
                                         struct spg_device_channel *out) {
    if (out == nullptr || (text_n > 0u && text == nullptr)) {
        return SPG_E_INVALID_ARG;
    }
    struct spg_sexpr_token tokens[DEVICE_TOKENS];
    struct spg_sexpr_node  nodes[DEVICE_NODES];
    size_t                 token_count = 0u;
    size_t                 node_count  = 0u;
    struct spg_sexpr_error error       = {};
    const enum spg_status  status =
        spg_sexpr_parse_text(text_n, text, DEVICE_TOKENS, tokens, DEVICE_NODES,
                             nodes, &token_count, &node_count, &error);
    if (status != SPG_OK) {
        return status;
    }
    if (node_count == 0u || nodes[0].kind != SPG_SEXPR_NODE_LIST) {
        return SPG_E_SCHEMA;
    }
    return parse_channel_node(text_n, text, nodes, 0u, out);
}

enum spg_status spg_device_load(const size_t text_n, const char text[],
                                struct spg_device *dev, size_t *out_bad) {
    if (dev == nullptr || (text_n > 0u && text == nullptr)) {
        return SPG_E_INVALID_ARG;
    }
    struct spg_sexpr_token tokens[DEVICE_TOKENS];
    struct spg_sexpr_node  nodes[DEVICE_NODES];
    size_t                 token_count = 0u;
    size_t                 node_count  = 0u;
    struct spg_sexpr_error error       = {};
    const enum spg_status  status =
        spg_sexpr_parse_text(text_n, text, DEVICE_TOKENS, tokens, DEVICE_NODES,
                             nodes, &token_count, &node_count, &error);
    if (status != SPG_OK) {
        return status;
    }
    if (node_count == 0u || nodes[0].kind != SPG_SEXPR_NODE_LIST) {
        return SPG_E_SCHEMA;
    }
    const uint32_t head = spg_sexpr_first_child(nodes, 0u);
    if (head == SPG_SEXPR_INVALID_INDEX ||
        nodes[head].kind != SPG_SEXPR_NODE_SYMBOL ||
        !spg_sexpr_span_eq_cstr(text_n, text, nodes[head].span, "device")) {
        return SPG_E_SCHEMA;
    }
    size_t index = 0u;
    for (uint32_t node = nodes[head].next_sibling;
         node != SPG_SEXPR_INVALID_INDEX;
         node = nodes[node].next_sibling, index += 1u) {
        struct spg_device_channel channel = {};
        enum spg_status           cstatus = SPG_E_SCHEMA;
        if (nodes[node].kind == SPG_SEXPR_NODE_LIST) {
            cstatus = parse_channel_node(text_n, text, nodes, node, &channel);
        }
        if (cstatus == SPG_OK) {
            cstatus = spg_device_add_channel(dev, &channel);
        }
        if (cstatus != SPG_OK) {
            if (out_bad != nullptr) {
                *out_bad = index;
            }
            return cstatus;
        }
    }
    return SPG_OK;
}

/* --- The transport: one program --------------------------------------- */

/* Run the channel's program, bounded: SPG_DEVICE_TIMEOUT_MS, then the whole
 * process group is killed — a silent machine must never stall the loop that
 * governs it. stderr is captured and dropped here; the device executor is
 * where failures become journal entries. */
static enum spg_status run_program(const char *program, const char *argument,
                                   const size_t stdout_cap,
                                   char        stdout_buf[]) {
    const char *argv[2] = {program, argument};
    char        stderr_buf[256];
    const struct spg_cmd_request request = {
        .argc       = argument == nullptr ? 1u : 2u,
        .argv       = argv,
        .timeout_ms = (uint64_t)SPG_DEVICE_TIMEOUT_MS,
        .limits     = SPG_CMD_DEFAULT_LIMITS,
        .stdout_cap = stdout_cap,
        .stdout_buf = stdout_buf,
        .stderr_cap = sizeof stderr_buf,
        .stderr_buf = stderr_buf,
    };
    struct spg_cmd_result result = {};
    const enum spg_status status = spg_cmd_executor_run(1u, &request, &result);
    if (status != SPG_OK || result.status != SPG_OK) {
        return SPG_E_IO;
    }
    if (!result.started || !result.exited || result.exit_code != 0) {
        return SPG_E_IO; /* covers the timeout: killed is not exited-0 */
    }
    return SPG_OK;
}

/* One integer, optionally wrapped in whitespace — anything else on stdout is
 * a program that did not keep the contract, and its number is not trusted. */
static bool parse_output(const char text[], int64_t *out) {
    const char *at = text;
    while (*at == ' ' || *at == '\t' || *at == '\n' || *at == '\r') {
        at += 1;
    }
    if (*at == '\0') {
        return false;
    }
    char         *end   = nullptr;
    const int64_t value = (int64_t)strtoll(at, &end, 10);
    if (end == at) {
        return false;
    }
    while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') {
        end += 1;
    }
    if (*end != '\0') {
        return false;
    }
    *out = value;
    return true;
}

enum spg_status spg_device_read(struct spg_device *dev, const char *name,
                                int64_t *out) {
    if (dev == nullptr || out == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    const struct spg_device_channel *channel = spg_device_find(dev, name);
    if (channel == nullptr) {
        return SPG_E_NOT_FOUND;
    }
    char                  output[64] = {};
    const enum spg_status status =
        run_program(channel->program, nullptr, sizeof output, output);
    if (status != SPG_OK) {
        return status;
    }
    int64_t value = 0;
    if (!parse_output(output, &value)) {
        return SPG_E_FORMAT;
    }
    *out = value;
    /* Contact is contact: a successful read keeps the watchdog fed, so a run
     * that only observes does not look like a machine gone silent. */
    dev->contact_pending = true;
    return SPG_OK;
}

enum spg_status spg_device_write(struct spg_device *dev, const char *name,
                                 const int64_t value) {
    if (dev == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    /* Every refusal is decided BEFORE the fork, so all of it can be tested
     * without a program that exists. A safety check reachable only once a
     * machine is plugged in is a safety check nobody runs. */
    const struct spg_device_channel *channel = spg_device_find(dev, name);
    if (channel == nullptr) {
        return SPG_E_NOT_FOUND;
    }
    if (!channel->writable) {
        return SPG_E_POLICY_DENIED;
    }
    if (value < channel->min || value > channel->max) {
        return SPG_E_LIMIT; /* refused, not clamped — see device.h */
    }
    char argument[24];
    (void)snprintf(argument, sizeof argument, "%lld", (long long)value);
    char                  output[64] = {};
    const enum spg_status status =
        run_program(channel->program, argument, sizeof output, output);
    if (status != SPG_OK) {
        return status;
    }
    int64_t echo = 0;
    if (parse_output(output, &echo) && echo != value) {
        /* The device acknowledged a different value than it was told. Treated
         * as a failure: a caller that believes its write landed will build the
         * next decision on a number the machine never accepted. */
        return SPG_E_IO;
    }
    dev->contact_pending = true;
    return SPG_OK;
}

/* --- The watchdog ----------------------------------------------------- */

void spg_device_arm_watchdog(struct spg_device *dev, const uint64_t timeout,
                             const uint64_t now) {
    if (dev == nullptr) {
        return;
    }
    dev->watchdog_timeout = timeout;
    dev->last_contact     = now;
    dev->contact_pending  = false;
}

enum spg_device_watchdog spg_device_watchdog_check(struct spg_device *dev,
                                                   const uint64_t     now) {
    if (dev == nullptr) {
        return SPG_WATCHDOG_DISABLED;
    }
    if (dev->contact_pending) {
        dev->contact_pending = false;
        dev->last_contact    = now;
    }
    if (dev->watchdog_timeout == 0u) {
        return SPG_WATCHDOG_DISABLED;
    }
    if (now < dev->last_contact) {
        /* Time moved backwards. Treated as contact rather than as expiry: an
         * injected timestamp going backwards is a caller bug, and tripping a
         * machine into its safe state on a bookkeeping error would be the more
         * expensive of the two wrong answers. */
        return SPG_WATCHDOG_OK;
    }
    return (now - dev->last_contact) > dev->watchdog_timeout
               ? SPG_WATCHDOG_EXPIRED
               : SPG_WATCHDOG_OK;
}

enum spg_status spg_device_safe_state(struct spg_device *dev) {
    if (dev == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    enum spg_status first_failure = SPG_OK;
    for (size_t i = 0u; i < dev->n_channels; i += 1u) {
        if (!dev->channels[i].writable) {
            continue;
        }
        const enum spg_status status =
            spg_device_write(dev, dev->channels[i].name, dev->channels[i].safe);
        if (status != SPG_OK && first_failure == SPG_OK) {
            first_failure = status; /* keep going: see device.h */
        }
    }
    return first_failure;
}

/* --- What the agent sees ----------------------------------------------- */

enum spg_status spg_device_sample(struct spg_device       *dev,
                                  struct spg_device_state *out) {
    if (dev == nullptr || out == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out = (struct spg_device_state){};
    enum spg_status first = SPG_OK;
    for (size_t i = 0u; i < dev->n_channels; i += 1u) {
        struct spg_device_reading *reading = &out->readings[out->n];
        memcpy(reading->name, dev->channels[i].name, sizeof reading->name);
        int64_t               value  = 0;
        const enum spg_status status = spg_device_read(dev, reading->name,
                                                       &value);
        if (status == SPG_OK) {
            reading->value = value;
            reading->known = true;
        } else if (first == SPG_OK) {
            /* Every channel is attempted even after one fails — one dead
             * sensor must not blind the agent to the rest of the plant. The
             * first failure is what the caller hears about. */
            first = status;
        }
        out->n += 1u;
    }
    return first;
}

enum spg_status spg_device_state_render(const struct spg_device_state *state,
                                        const size_t dst_capacity,
                                        char dst[static dst_capacity],
                                        size_t *out_required) {
    if (state == nullptr || dst == nullptr || out_required == nullptr ||
        dst_capacity == 0u || state->n > SPG_DEVICE_MAX_CHANNELS) {
        return SPG_E_INVALID_ARG;
    }
    /* SPG_DEVICE_RENDER_CAP bounds the worst case by construction, so the
     * whole block is rendered here first and copied only if it fits — no
     * partial record ever reaches dst. */
    char   block[SPG_DEVICE_RENDER_CAP];
    size_t used = (size_t)snprintf(block, sizeof block, "(device-state");
    for (size_t i = 0u; i < state->n; i += 1u) {
        const struct spg_device_reading *r = &state->readings[i];
        used += r->known
                    ? (size_t)snprintf(block + used, sizeof block - used,
                                       " (%s %lld)", r->name,
                                       (long long)r->value)
                    /* Never 0 for a reading that was not taken — device.h. */
                    : (size_t)snprintf(block + used, sizeof block - used,
                                       " (%s unknown)", r->name);
    }
    used += (size_t)snprintf(block + used, sizeof block - used, ")");
    *out_required = used + 1u;
    if (used + 1u > dst_capacity) {
        return SPG_E_LIMIT; /* no partial record: dst is left untrusted */
    }
    memcpy(dst, block, used + 1u);
    return SPG_OK;
}
