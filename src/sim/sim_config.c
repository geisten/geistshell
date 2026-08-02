#include "geistshell/sim_config.h"

#include "geistshell/schema.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static const struct spg_schema_field_rule scenario_fields[] = {
    {.name = "host", .value_kind = SPG_SCHEMA_VALUE_ANY, .min_values = 2u, .max_values = 2u, .required = false, .unique = false},
    {.name = "service", .value_kind = SPG_SCHEMA_VALUE_ANY, .min_values = 5u, .max_values = 5u, .required = false, .unique = false},
    {.name = "account", .value_kind = SPG_SCHEMA_VALUE_ANY, .min_values = 4u, .max_values = 4u, .required = false, .unique = false},
    {.name = "credential", .value_kind = SPG_SCHEMA_VALUE_ANY, .min_values = 3u, .max_values = 3u, .required = false, .unique = false},
    {.name = "vulnerability", .value_kind = SPG_SCHEMA_VALUE_ANY, .min_values = 4u, .max_values = 4u, .required = false, .unique = false},
    {.name = "network_edge", .value_kind = SPG_SCHEMA_VALUE_ANY, .min_values = 3u, .max_values = 3u, .required = false, .unique = false},
};

static const struct spg_schema_form_rule scenario_forms[] = {
    {.name                 = "scenario",
     .field_rule_count     = sizeof(scenario_fields) / sizeof(scenario_fields[0]),
     .field_rules          = scenario_fields,
     .allow_unknown_fields = false,
     .min_fields           = 1u,
     .max_fields           = SPG_SIM_MAX_HOSTS + SPG_SIM_MAX_SERVICES +
                   SPG_SIM_MAX_ACCOUNTS + SPG_SIM_MAX_CREDENTIALS +
                   SPG_SIM_MAX_VULNERABILITIES + SPG_SIM_MAX_NETWORK_EDGES},
};

static const struct spg_schema scenario_schema = {
    .form_rule_count               = sizeof(scenario_forms) / sizeof(scenario_forms[0]),
    .form_rules                    = scenario_forms,
    .allow_unknown_top_level_forms = false,
    .max_top_level_forms           = 1u,
    .max_depth                     = 4u,
};

static void set_error(struct spg_sim_config_error *error,
                      const enum spg_status       status,
                      const uint32_t              node_index,
                      const size_t                offset) {
    if (error == nullptr) {
        return;
    }
    error->status     = status;
    error->node_index = node_index;
    error->offset     = offset;
}

static bool span_same(const size_t input_n, const char input[],
                      const struct spg_text_span a,
                      const struct spg_text_span b) {
    if (input == nullptr || !spg_sexpr_span_valid(input_n, a) || !spg_sexpr_span_valid(input_n, b) ||
        a.length != b.length) {
        return false;
    }
    return memcmp(input + a.offset, input + b.offset, a.length) == 0;
}

static size_t node_offset_or(const struct spg_sexpr_node nodes[static 1],
                             const uint32_t node,
                             const size_t fallback_offset) {
    return node == SPG_SEXPR_INVALID_INDEX ? fallback_offset
                                           : nodes[node].span.offset;
}

static uint32_t find_obj_value(const size_t input_n, const char input[],
                               const struct spg_sexpr_node nodes[static 1],
                               const uint32_t obj,
                               const char *field_name) {
    uint32_t field = spg_sexpr_second_child(nodes, obj);
    while (field != SPG_SEXPR_INVALID_INDEX) {
        const uint32_t name = spg_sexpr_first_child(nodes, field);
        const uint32_t val  = spg_sexpr_second_child(nodes, field);
        if (name != SPG_SEXPR_INVALID_INDEX && val != SPG_SEXPR_INVALID_INDEX &&
            nodes[name].kind == SPG_SEXPR_NODE_SYMBOL &&
            spg_sexpr_span_eq_cstr(input_n, input, nodes[name].span, field_name)) {
            return val;
        }
        field = nodes[field].next_sibling;
    }
    return SPG_SEXPR_INVALID_INDEX;
}

static enum spg_status parse_uint32_span(const size_t input_n,
                                         const char input[],
                                         const struct spg_text_span span,
                                         uint32_t *out) {
    if (input == nullptr || out == nullptr || span.length == 0u ||
        !spg_sexpr_span_valid(input_n, span)) {
        return SPG_E_INVALID_ARG;
    }
    uint64_t value = 0u;
    for (size_t i = 0u; i < span.length; i += 1u) {
        const char c = input[span.offset + i];
        if (c < '0' || c > '9') {
            return SPG_E_FORMAT;
        }
        value = (value * 10u) + (uint64_t)(c - '0');
        if (value > UINT32_MAX) {
            return SPG_E_OVERFLOW;
        }
    }
    *out = (uint32_t)value;
    return SPG_OK;
}

static enum spg_status parse_bp(const size_t input_n, const char input[],
                                const struct spg_sexpr_node nodes[static 1],
                                const uint32_t value_node,
                                uint32_t *out) {
    if (value_node == SPG_SEXPR_INVALID_INDEX ||
        nodes[value_node].kind != SPG_SEXPR_NODE_SYMBOL) {
        return SPG_E_SCHEMA;
    }
    const enum spg_status status =
        parse_uint32_span(input_n, input, nodes[value_node].span, out);
    if (status != SPG_OK) {
        return status;
    }
    return *out <= SPG_SIM_MAX_BASIS_POINTS ? SPG_OK : SPG_E_LIMIT;
}

static enum spg_status parse_bool(const size_t input_n, const char input[],
                                  const struct spg_text_span span,
                                  bool *out) {
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

static enum spg_status require_symbol_value(
    const struct spg_sexpr_node nodes[static 1], const uint32_t value_node) {
    return value_node != SPG_SEXPR_INVALID_INDEX &&
                   nodes[value_node].kind == SPG_SEXPR_NODE_SYMBOL
               ? SPG_OK
               : SPG_E_SCHEMA;
}

static int32_t find_host(const size_t input_n, const char input[],
                         const struct spg_sim_config *config,
                         const struct spg_text_span id) {
    for (size_t i = 0u; i < config->host_count; i += 1u) {
        if (span_same(input_n, input, config->hosts[i].id, id)) {
            return (int32_t)i;
        }
    }
    return -1;
}

static int32_t find_service(const size_t input_n, const char input[],
                            const struct spg_sim_config *config,
                            const struct spg_text_span id) {
    for (size_t i = 0u; i < config->service_count; i += 1u) {
        if (span_same(input_n, input, config->services[i].id, id)) {
            return (int32_t)i;
        }
    }
    return -1;
}

static int32_t find_account(const size_t input_n, const char input[],
                            const struct spg_sim_config *config,
                            const struct spg_text_span id) {
    for (size_t i = 0u; i < config->account_count; i += 1u) {
        if (span_same(input_n, input, config->accounts[i].id, id)) {
            return (int32_t)i;
        }
    }
    return -1;
}

static enum spg_status load_host(const size_t input_n, const char input[],
                                 const struct spg_sexpr_node nodes[static 1],
                                 const uint32_t obj,
                                 struct spg_sim_config *out,
                                 struct spg_sim_config_error *error) {
    if (out->host_count >= SPG_SIM_MAX_HOSTS) {
        set_error(error, SPG_E_LIMIT, obj, nodes[obj].span.offset);
        return SPG_E_LIMIT;
    }
    const uint32_t id_node = find_obj_value(input_n, input, nodes, obj, "id");
    const uint32_t crit_node =
        find_obj_value(input_n, input, nodes, obj, "criticality_bp");
    if (require_symbol_value(nodes, id_node) != SPG_OK) {
        set_error(error, SPG_E_SCHEMA, obj, nodes[obj].span.offset);
        return SPG_E_SCHEMA;
    }
    if (find_host(input_n, input, out, nodes[id_node].span) >= 0) {
        set_error(error, SPG_E_SCHEMA, id_node, nodes[id_node].span.offset);
        return SPG_E_SCHEMA;
    }
    uint32_t bp = 0u;
    enum spg_status status = parse_bp(input_n, input, nodes, crit_node, &bp);
    if (status != SPG_OK) {
        set_error(error, status, crit_node,
                  node_offset_or(nodes, crit_node, nodes[obj].span.offset));
        return status;
    }
    out->hosts[out->host_count] = (struct spg_sim_host){
        .id             = nodes[id_node].span,
        .criticality_bp = bp,
    };
    out->host_count += 1u;
    return SPG_OK;
}

static enum spg_status load_service(const size_t input_n, const char input[],
                                    const struct spg_sexpr_node nodes[static 1],
                                    const uint32_t obj,
                                    struct spg_sim_config *out,
                                    struct spg_sim_config_error *error) {
    if (out->service_count >= SPG_SIM_MAX_SERVICES) {
        set_error(error, SPG_E_LIMIT, obj, nodes[obj].span.offset);
        return SPG_E_LIMIT;
    }
    const uint32_t id_node = find_obj_value(input_n, input, nodes, obj, "id");
    const uint32_t host_node = find_obj_value(input_n, input, nodes, obj, "host");
    const uint32_t name_node = find_obj_value(input_n, input, nodes, obj, "name");
    const uint32_t port_node = find_obj_value(input_n, input, nodes, obj, "port");
    const uint32_t exp_node =
        find_obj_value(input_n, input, nodes, obj, "exposure_bp");
    if (require_symbol_value(nodes, id_node) != SPG_OK ||
        require_symbol_value(nodes, host_node) != SPG_OK ||
        require_symbol_value(nodes, name_node) != SPG_OK ||
        require_symbol_value(nodes, port_node) != SPG_OK) {
        set_error(error, SPG_E_SCHEMA, obj, nodes[obj].span.offset);
        return SPG_E_SCHEMA;
    }
    if (find_service(input_n, input, out, nodes[id_node].span) >= 0) {
        set_error(error, SPG_E_SCHEMA, id_node, nodes[id_node].span.offset);
        return SPG_E_SCHEMA;
    }
    const int32_t host_index = find_host(input_n, input, out, nodes[host_node].span);
    if (host_index < 0) {
        set_error(error, SPG_E_SCHEMA, host_node, nodes[host_node].span.offset);
        return SPG_E_SCHEMA;
    }
    uint32_t port = 0u;
    enum spg_status status =
        parse_uint32_span(input_n, input, nodes[port_node].span, &port);
    if (status != SPG_OK || port > 65535u) {
        set_error(error, status == SPG_OK ? SPG_E_LIMIT : status, port_node,
                  nodes[port_node].span.offset);
        return status == SPG_OK ? SPG_E_LIMIT : status;
    }
    uint32_t exposure = 0u;
    status = parse_bp(input_n, input, nodes, exp_node, &exposure);
    if (status != SPG_OK) {
        set_error(error, status, exp_node,
                  node_offset_or(nodes, exp_node, nodes[obj].span.offset));
        return status;
    }
    out->services[out->service_count] = (struct spg_sim_service){
        .id          = nodes[id_node].span,
        .host_index  = (uint32_t)host_index,
        .name        = nodes[name_node].span,
        .port        = port,
        .exposure_bp = exposure,
    };
    out->service_count += 1u;
    return SPG_OK;
}

static enum spg_status load_account(const size_t input_n, const char input[],
                                    const struct spg_sexpr_node nodes[static 1],
                                    const uint32_t obj,
                                    struct spg_sim_config *out,
                                    struct spg_sim_config_error *error) {
    if (out->account_count >= SPG_SIM_MAX_ACCOUNTS) {
        set_error(error, SPG_E_LIMIT, obj, nodes[obj].span.offset);
        return SPG_E_LIMIT;
    }
    const uint32_t id_node = find_obj_value(input_n, input, nodes, obj, "id");
    const uint32_t host_node = find_obj_value(input_n, input, nodes, obj, "host");
    const uint32_t user_node =
        find_obj_value(input_n, input, nodes, obj, "username");
    const uint32_t enabled_node =
        find_obj_value(input_n, input, nodes, obj, "enabled");
    if (require_symbol_value(nodes, id_node) != SPG_OK ||
        require_symbol_value(nodes, host_node) != SPG_OK ||
        require_symbol_value(nodes, user_node) != SPG_OK ||
        require_symbol_value(nodes, enabled_node) != SPG_OK) {
        set_error(error, SPG_E_SCHEMA, obj, nodes[obj].span.offset);
        return SPG_E_SCHEMA;
    }
    if (find_account(input_n, input, out, nodes[id_node].span) >= 0) {
        set_error(error, SPG_E_SCHEMA, id_node, nodes[id_node].span.offset);
        return SPG_E_SCHEMA;
    }
    const int32_t host_index = find_host(input_n, input, out, nodes[host_node].span);
    if (host_index < 0) {
        set_error(error, SPG_E_SCHEMA, host_node, nodes[host_node].span.offset);
        return SPG_E_SCHEMA;
    }
    bool enabled = false;
    const enum spg_status status =
        parse_bool(input_n, input, nodes[enabled_node].span, &enabled);
    if (status != SPG_OK) {
        set_error(error, status, enabled_node, nodes[enabled_node].span.offset);
        return status;
    }
    out->accounts[out->account_count] = (struct spg_sim_account){
        .id         = nodes[id_node].span,
        .host_index = (uint32_t)host_index,
        .username   = nodes[user_node].span,
        .enabled    = enabled,
    };
    out->account_count += 1u;
    return SPG_OK;
}

static bool credential_id_exists(const size_t input_n, const char input[],
                                 const struct spg_sim_config *out,
                                 const struct spg_text_span id) {
    for (size_t i = 0u; i < out->credential_count; i += 1u) {
        if (span_same(input_n, input, out->credentials[i].id, id)) {
            return true;
        }
    }
    return false;
}

static enum spg_status load_credential(const size_t input_n, const char input[],
                                       const struct spg_sexpr_node nodes[static 1],
                                       const uint32_t obj,
                                       struct spg_sim_config *out,
                                       struct spg_sim_config_error *error) {
    if (out->credential_count >= SPG_SIM_MAX_CREDENTIALS) {
        set_error(error, SPG_E_LIMIT, obj, nodes[obj].span.offset);
        return SPG_E_LIMIT;
    }
    const uint32_t id_node = find_obj_value(input_n, input, nodes, obj, "id");
    const uint32_t account_node =
        find_obj_value(input_n, input, nodes, obj, "account");
    const uint32_t strength_node =
        find_obj_value(input_n, input, nodes, obj, "strength_bp");
    if (require_symbol_value(nodes, id_node) != SPG_OK ||
        require_symbol_value(nodes, account_node) != SPG_OK) {
        set_error(error, SPG_E_SCHEMA, obj, nodes[obj].span.offset);
        return SPG_E_SCHEMA;
    }
    if (credential_id_exists(input_n, input, out, nodes[id_node].span)) {
        set_error(error, SPG_E_SCHEMA, id_node, nodes[id_node].span.offset);
        return SPG_E_SCHEMA;
    }
    const int32_t account_index =
        find_account(input_n, input, out, nodes[account_node].span);
    if (account_index < 0) {
        set_error(error, SPG_E_SCHEMA, account_node, nodes[account_node].span.offset);
        return SPG_E_SCHEMA;
    }
    uint32_t strength = 0u;
    enum spg_status status =
        parse_bp(input_n, input, nodes, strength_node, &strength);
    if (status != SPG_OK) {
        set_error(error, status, strength_node,
                  node_offset_or(nodes, strength_node, nodes[obj].span.offset));
        return status;
    }
    out->credentials[out->credential_count] = (struct spg_sim_credential){
        .id            = nodes[id_node].span,
        .account_index = (uint32_t)account_index,
        .strength_bp   = strength,
    };
    out->credential_count += 1u;
    return SPG_OK;
}

static bool vulnerability_id_exists(const size_t input_n, const char input[],
                                    const struct spg_sim_config *out,
                                    const struct spg_text_span id) {
    for (size_t i = 0u; i < out->vulnerability_count; i += 1u) {
        if (span_same(input_n, input, out->vulnerabilities[i].id, id)) {
            return true;
        }
    }
    return false;
}

static enum spg_status load_vulnerability(
    const size_t input_n, const char input[],
    const struct spg_sexpr_node nodes[static 1], const uint32_t obj,
    struct spg_sim_config *out, struct spg_sim_config_error *error) {
    if (out->vulnerability_count >= SPG_SIM_MAX_VULNERABILITIES) {
        set_error(error, SPG_E_LIMIT, obj, nodes[obj].span.offset);
        return SPG_E_LIMIT;
    }
    const uint32_t id_node = find_obj_value(input_n, input, nodes, obj, "id");
    const uint32_t service_node =
        find_obj_value(input_n, input, nodes, obj, "service");
    const uint32_t sev_node =
        find_obj_value(input_n, input, nodes, obj, "severity_bp");
    const uint32_t patched_node =
        find_obj_value(input_n, input, nodes, obj, "patched");
    if (require_symbol_value(nodes, id_node) != SPG_OK ||
        require_symbol_value(nodes, service_node) != SPG_OK ||
        require_symbol_value(nodes, patched_node) != SPG_OK) {
        set_error(error, SPG_E_SCHEMA, obj, nodes[obj].span.offset);
        return SPG_E_SCHEMA;
    }
    if (vulnerability_id_exists(input_n, input, out, nodes[id_node].span)) {
        set_error(error, SPG_E_SCHEMA, id_node, nodes[id_node].span.offset);
        return SPG_E_SCHEMA;
    }
    const int32_t service_index =
        find_service(input_n, input, out, nodes[service_node].span);
    if (service_index < 0) {
        set_error(error, SPG_E_SCHEMA, service_node, nodes[service_node].span.offset);
        return SPG_E_SCHEMA;
    }
    uint32_t severity = 0u;
    enum spg_status status = parse_bp(input_n, input, nodes, sev_node, &severity);
    if (status != SPG_OK) {
        set_error(error, status, sev_node,
                  node_offset_or(nodes, sev_node, nodes[obj].span.offset));
        return status;
    }
    bool patched = false;
    status = parse_bool(input_n, input, nodes[patched_node].span, &patched);
    if (status != SPG_OK) {
        set_error(error, status, patched_node, nodes[patched_node].span.offset);
        return status;
    }
    out->vulnerabilities[out->vulnerability_count] =
        (struct spg_sim_vulnerability){
            .id            = nodes[id_node].span,
            .service_index = (uint32_t)service_index,
            .severity_bp   = severity,
            .patched       = patched,
        };
    out->vulnerability_count += 1u;
    return SPG_OK;
}

static enum spg_status load_network_edge(
    const size_t input_n, const char input[],
    const struct spg_sexpr_node nodes[static 1], const uint32_t obj,
    struct spg_sim_config *out, struct spg_sim_config_error *error) {
    if (out->network_edge_count >= SPG_SIM_MAX_NETWORK_EDGES) {
        set_error(error, SPG_E_LIMIT, obj, nodes[obj].span.offset);
        return SPG_E_LIMIT;
    }
    const uint32_t from_node =
        find_obj_value(input_n, input, nodes, obj, "from");
    const uint32_t to_node = find_obj_value(input_n, input, nodes, obj, "to");
    const uint32_t reach_node =
        find_obj_value(input_n, input, nodes, obj, "reachability_bp");
    if (require_symbol_value(nodes, from_node) != SPG_OK ||
        require_symbol_value(nodes, to_node) != SPG_OK) {
        set_error(error, SPG_E_SCHEMA, obj, nodes[obj].span.offset);
        return SPG_E_SCHEMA;
    }
    const int32_t from = find_host(input_n, input, out, nodes[from_node].span);
    const int32_t to   = find_host(input_n, input, out, nodes[to_node].span);
    if (from < 0 || to < 0) {
        const uint32_t bad = from < 0 ? from_node : to_node;
        set_error(error, SPG_E_SCHEMA, bad, nodes[bad].span.offset);
        return SPG_E_SCHEMA;
    }
    uint32_t reachability = 0u;
    const enum spg_status status =
        parse_bp(input_n, input, nodes, reach_node, &reachability);
    if (status != SPG_OK) {
        set_error(error, status, reach_node,
                  node_offset_or(nodes, reach_node, nodes[obj].span.offset));
        return status;
    }
    out->network_edges[out->network_edge_count] =
        (struct spg_sim_network_edge){
            .from_host_index = (uint32_t)from,
            .to_host_index   = (uint32_t)to,
            .reachability_bp = reachability,
        };
    out->network_edge_count += 1u;
    return SPG_OK;
}

static enum spg_status load_object(const size_t input_n, const char input[],
                                   const struct spg_sexpr_node nodes[static 1],
                                   const uint32_t obj,
                                   struct spg_sim_config *out,
                                   struct spg_sim_config_error *error) {
    const uint32_t name = spg_sexpr_first_child(nodes, obj);
    if (name == SPG_SEXPR_INVALID_INDEX ||
        nodes[name].kind != SPG_SEXPR_NODE_SYMBOL) {
        set_error(error, SPG_E_SCHEMA, obj, nodes[obj].span.offset);
        return SPG_E_SCHEMA;
    }
    if (spg_sexpr_span_eq_cstr(input_n, input, nodes[name].span, "host")) {
        return load_host(input_n, input, nodes, obj, out, error);
    }
    if (spg_sexpr_span_eq_cstr(input_n, input, nodes[name].span, "service")) {
        return load_service(input_n, input, nodes, obj, out, error);
    }
    if (spg_sexpr_span_eq_cstr(input_n, input, nodes[name].span, "account")) {
        return load_account(input_n, input, nodes, obj, out, error);
    }
    if (spg_sexpr_span_eq_cstr(input_n, input, nodes[name].span, "credential")) {
        return load_credential(input_n, input, nodes, obj, out, error);
    }
    if (spg_sexpr_span_eq_cstr(input_n, input, nodes[name].span, "vulnerability")) {
        return load_vulnerability(input_n, input, nodes, obj, out, error);
    }
    if (spg_sexpr_span_eq_cstr(input_n, input, nodes[name].span, "network_edge")) {
        return load_network_edge(input_n, input, nodes, obj, out, error);
    }
    set_error(error, SPG_E_SCHEMA, name, nodes[name].span.offset);
    return SPG_E_SCHEMA;
}

enum spg_status spg_sim_config_load(
    const size_t input_n, const char input[], const size_t token_capacity,
    struct spg_sexpr_token tokens[static token_capacity],
    const size_t node_capacity,
    struct spg_sexpr_node nodes[static node_capacity],
    struct spg_sim_config *out, struct spg_sim_config_error *error) {
    if (input == nullptr || tokens == nullptr || nodes == nullptr ||
        out == nullptr || token_capacity == 0u || node_capacity == 0u) {
        set_error(error, SPG_E_INVALID_ARG, SPG_SEXPR_INVALID_INDEX, 0u);
        return SPG_E_INVALID_ARG;
    }
    *out = (struct spg_sim_config){};
    set_error(error, SPG_OK, SPG_SEXPR_INVALID_INDEX, 0u);

    struct spg_sexpr_error parse_error = {};
    size_t                 token_count = 0u;
    size_t                 node_count  = 0u;
    enum spg_status status =
        spg_sexpr_parse_text(input_n, input, token_capacity, tokens,
                             node_capacity, nodes, &token_count, &node_count,
                             &parse_error);
    if (status != SPG_OK) {
        set_error(error, status, SPG_SEXPR_INVALID_INDEX, parse_error.offset);
        return status;
    }
    if (node_count == 0u) {
        set_error(error, SPG_E_SCHEMA, SPG_SEXPR_INVALID_INDEX, 0u);
        return SPG_E_SCHEMA;
    }

    struct spg_schema_error schema_error = {};
    status = spg_schema_validate(input_n, input, node_count, nodes,
                                 &scenario_schema, &schema_error);
    if (status != SPG_OK) {
        set_error(error, status, schema_error.node_index, schema_error.offset);
        return status;
    }

    const uint32_t scenario = 0u;
    const uint32_t form_name = spg_sexpr_first_child(nodes, scenario);
    uint32_t obj = nodes[form_name].next_sibling;
    while (obj != SPG_SEXPR_INVALID_INDEX) {
        status = load_object(input_n, input, nodes, obj, out, error);
        if (status != SPG_OK) {
            *out = (struct spg_sim_config){};
            return status;
        }
        obj = nodes[obj].next_sibling;
    }
    if (out->host_count == 0u) {
        set_error(error, SPG_E_SCHEMA, scenario, nodes[scenario].span.offset);
        return SPG_E_SCHEMA;
    }
    return SPG_OK;
}
