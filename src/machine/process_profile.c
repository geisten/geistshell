/* (process-profile ...) — parsed with the shared s-expression reader, in the
 * same shape as policy_config.c so there is one DSL, not two. */

#include "geistshell/process_profile.h"

#include <string.h>

/* Kernel comm length minus the NUL: a longer match string can only ever equal
 * the truncated name, never the full one. */
static constexpr size_t comm_visible = SPG_PROCESS_NAME_CAP - 1u;

static void set_error(struct spg_process_profile_error *error,
                      const enum spg_status status, const uint32_t node_index,
                      const size_t offset) {
    if (error == nullptr) {
        return;
    }
    error->status     = status;
    error->node_index = node_index;
    error->offset     = offset;
}

/* Copy a span's payload into a fixed field. Rejects anything that does not fit
 * rather than truncating: a silently shortened match string would match the
 * wrong process. */
static bool copy_span(const size_t input_n, const char input[],
                      const struct spg_text_span span, const size_t cap,
                      char dst[]) {
    if (!spg_sexpr_span_valid(input_n, span) || span.length + 1u > cap) {
        return false;
    }
    memcpy(dst, input + span.offset, span.length);
    dst[span.length] = '\0';
    return true;
}

static uint32_t field_value(const size_t input_n, const char input[],
                            const struct spg_sexpr_node nodes[static 1],
                            const uint32_t entry, const char *name) {
    uint32_t field = spg_sexpr_first_child(nodes, entry);
    while (field != SPG_SEXPR_INVALID_INDEX) {
        if (nodes[field].kind == SPG_SEXPR_NODE_LIST) {
            const uint32_t field_name = spg_sexpr_first_child(nodes, field);
            if (field_name != SPG_SEXPR_INVALID_INDEX &&
                spg_sexpr_span_eq_cstr(input_n, input, nodes[field_name].span,
                                       name)) {
                return spg_sexpr_second_child(nodes, field);
            }
        }
        field = nodes[field].next_sibling;
    }
    return SPG_SEXPR_INVALID_INDEX;
}

static enum spg_status parse_bool(const size_t input_n, const char input[],
                                  const struct spg_text_span span, bool *out) {
    if (spg_sexpr_span_eq_cstr(input_n, input, span, "true")) {
        *out = true;
        return SPG_OK;
    }
    if (spg_sexpr_span_eq_cstr(input_n, input, span, "false")) {
        *out = false;
        return SPG_OK;
    }
    return SPG_E_SCHEMA;
}

static enum spg_status parse_role(const size_t input_n, const char input[],
                                  const struct spg_text_span span,
                                  enum spg_process_role     *out) {
    if (spg_sexpr_span_eq_cstr(input_n, input, span, "critical")) {
        *out = SPG_PROCESS_ROLE_CRITICAL;
        return SPG_OK;
    }
    if (spg_sexpr_span_eq_cstr(input_n, input, span, "batch")) {
        *out = SPG_PROCESS_ROLE_BATCH;
        return SPG_OK;
    }
    /* An unrecognised role is rejected, not silently downgraded to unknown:
     * a typo in `criticl` must not turn a protected process into a pausable
     * one. */
    return SPG_E_SCHEMA;
}

static enum spg_status parse_entry(const size_t input_n, const char input[],
                                   const struct spg_sexpr_node nodes[static 1],
                                   const uint32_t              entry,
                                   struct spg_process_profile_entry *out,
                                   struct spg_process_profile_error *error) {
    *out = (struct spg_process_profile_entry){};

    const uint32_t       id_node = spg_sexpr_second_child(nodes, entry);
    struct spg_text_span id_span = {};
    if (id_node == SPG_SEXPR_INVALID_INDEX ||
        !spg_sexpr_string_payload_span(&nodes[id_node], &id_span) ||
        !copy_span(input_n, input, id_span, SPG_PROCESS_ID_CAP, out->id) ||
        out->id[0] == '\0') {
        set_error(error, SPG_E_SCHEMA, entry, nodes[entry].span.offset);
        return SPG_E_SCHEMA;
    }

    const uint32_t match_node =
        field_value(input_n, input, nodes, entry, "match");
    struct spg_text_span match_span = {};
    if (match_node == SPG_SEXPR_INVALID_INDEX ||
        !spg_sexpr_string_payload_span(&nodes[match_node], &match_span) ||
        !copy_span(input_n, input, match_span, SPG_PROCESS_MATCH_CAP,
                   out->match) ||
        out->match[0] == '\0') {
        set_error(error, SPG_E_SCHEMA, entry, nodes[entry].span.offset);
        return SPG_E_SCHEMA;
    }

    const uint32_t role_node =
        field_value(input_n, input, nodes, entry, "role");
    if (role_node == SPG_SEXPR_INVALID_INDEX ||
        parse_role(input_n, input, nodes[role_node].span, &out->role) !=
            SPG_OK) {
        set_error(error, SPG_E_SCHEMA, entry, nodes[entry].span.offset);
        return SPG_E_SCHEMA;
    }

    /* Both permissions are required and default to nothing: a profile that
     * forgets may_pause must not silently grant it. */
    const uint32_t pause_node =
        field_value(input_n, input, nodes, entry, "may_pause");
    const uint32_t stop_node =
        field_value(input_n, input, nodes, entry, "may_stop");
    if (pause_node == SPG_SEXPR_INVALID_INDEX ||
        stop_node == SPG_SEXPR_INVALID_INDEX ||
        parse_bool(input_n, input, nodes[pause_node].span, &out->may_pause) !=
            SPG_OK ||
        parse_bool(input_n, input, nodes[stop_node].span, &out->may_stop) !=
            SPG_OK) {
        set_error(error, SPG_E_SCHEMA, entry, nodes[entry].span.offset);
        return SPG_E_SCHEMA;
    }

    /* A critical process that may be stopped is a contradiction the profile
     * author did not mean; refuse it here rather than let phase 6 resolve it.
     */
    if (out->role == SPG_PROCESS_ROLE_CRITICAL &&
        (out->may_pause || out->may_stop)) {
        set_error(error, SPG_E_SCHEMA, entry, nodes[entry].span.offset);
        return SPG_E_SCHEMA;
    }
    return SPG_OK;
}

enum spg_status spg_process_profile_load(
    const size_t input_n, const char input[], const size_t token_capacity,
    struct spg_sexpr_token      tokens[static token_capacity],
    const size_t                node_capacity,
    struct spg_sexpr_node       nodes[static node_capacity],
    struct spg_process_profile *out, struct spg_process_profile_error *error) {
    if (out == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out = (struct spg_process_profile){};
    set_error(error, SPG_OK, SPG_SEXPR_INVALID_INDEX, 0u);

    size_t                 token_count = 0u;
    size_t                 node_count  = 0u;
    struct spg_sexpr_error parse_error = {};
    const enum spg_status  status      = spg_sexpr_parse_text(
        input_n, input, token_capacity, tokens, node_capacity, nodes,
        &token_count, &node_count, &parse_error);
    if (status != SPG_OK) {
        set_error(error, status, SPG_SEXPR_INVALID_INDEX, parse_error.offset);
        return status;
    }
    if (node_count == 0u) {
        return SPG_OK; /* empty file: nothing managed, still valid */
    }

    const uint32_t root = 0u;
    const uint32_t head = spg_sexpr_first_child(nodes, root);
    if (nodes[root].kind != SPG_SEXPR_NODE_LIST ||
        head == SPG_SEXPR_INVALID_INDEX ||
        !spg_sexpr_span_eq_cstr(input_n, input, nodes[head].span,
                                "process-profile")) {
        set_error(error, SPG_E_SCHEMA, root, nodes[root].span.offset);
        return SPG_E_SCHEMA;
    }

    uint32_t entry = nodes[head].next_sibling;
    while (entry != SPG_SEXPR_INVALID_INDEX) {
        const uint32_t entry_head = spg_sexpr_first_child(nodes, entry);
        if (nodes[entry].kind != SPG_SEXPR_NODE_LIST ||
            entry_head == SPG_SEXPR_INVALID_INDEX ||
            !spg_sexpr_span_eq_cstr(input_n, input, nodes[entry_head].span,
                                    "process")) {
            set_error(error, SPG_E_SCHEMA, entry, nodes[entry].span.offset);
            return SPG_E_SCHEMA;
        }
        if (out->count >= SPG_PROCESS_PROFILE_CAP) {
            set_error(error, SPG_E_LIMIT, entry, nodes[entry].span.offset);
            return SPG_E_LIMIT;
        }
        struct spg_process_profile_entry parsed = {};
        const enum spg_status            entry_status =
            parse_entry(input_n, input, nodes, entry, &parsed, error);
        if (entry_status != SPG_OK) {
            return entry_status;
        }
        for (size_t i = 0u; i < out->count; i += 1u) {
            if (strcmp(out->entries[i].id, parsed.id) == 0) {
                set_error(error, SPG_E_SCHEMA, entry, nodes[entry].span.offset);
                return SPG_E_SCHEMA;
            }
        }
        out->entries[out->count] = parsed;
        out->count += 1u;
        entry = nodes[entry].next_sibling;
    }
    return SPG_OK;
}

uint32_t spg_process_profile_match(const struct spg_process_profile *profile,
                                   const char                       *name) {
    if (profile == nullptr || name == nullptr || name[0] == '\0') {
        return SPG_PROCESS_NO_PROFILE;
    }
    for (size_t i = 0u; i < profile->count; i += 1u) {
        const char *match = profile->entries[i].match;
        if (strcmp(match, name) == 0) {
            return (uint32_t)i;
        }
        /* The kernel cut the name at 15 characters; compare what survived. */
        if (strlen(match) > comm_visible &&
            strncmp(match, name, comm_visible) == 0 &&
            strlen(name) == comm_visible) {
            return (uint32_t)i;
        }
    }
    return SPG_PROCESS_NO_PROFILE;
}

void spg_process_apply_profile(const struct spg_process_profile *profile,
                               const size_t                      n,
                               struct spg_process_sample         procs[]) {
    for (size_t i = 0u; i < n; i += 1u) {
        const uint32_t index =
            spg_process_profile_match(profile, procs[i].name);
        procs[i].profile_index = index;
        if (index == SPG_PROCESS_NO_PROFILE) {
            procs[i].role          = SPG_PROCESS_ROLE_UNKNOWN;
            procs[i].may_pause     = false;
            procs[i].may_stop      = false;
            procs[i].profile_id[0] = '\0';
            continue;
        }
        memcpy(procs[i].profile_id, profile->entries[index].id,
               SPG_PROCESS_ID_CAP);
        procs[i].role      = profile->entries[index].role;
        procs[i].may_pause = profile->entries[index].may_pause;
        procs[i].may_stop  = profile->entries[index].may_stop;
    }
}
