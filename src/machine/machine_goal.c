/* Goals, and the only honest way to tell whether one was met: measure the
 * machine, not the model's account of it. */

#include "geistshell/machine_goal.h"

#include <string.h>

static uint32_t field_value(const size_t input_n, const char input[],
                            const struct spg_sexpr_node nodes[static 1],
                            const uint32_t parent, const char *name) {
    uint32_t field = spg_sexpr_first_child(nodes, parent);
    while (field != SPG_SEXPR_INVALID_INDEX) {
        if (nodes[field].kind == SPG_SEXPR_NODE_LIST) {
            const uint32_t head = spg_sexpr_first_child(nodes, field);
            if (head != SPG_SEXPR_INVALID_INDEX &&
                spg_sexpr_span_eq_cstr(input_n, input, nodes[head].span,
                                       name)) {
                return spg_sexpr_second_child(nodes, field);
            }
        }
        field = nodes[field].next_sibling;
    }
    return SPG_SEXPR_INVALID_INDEX;
}

enum spg_status
spg_machine_goal_load(const size_t input_n, const char input[],
                      const size_t             token_capacity,
                      struct spg_sexpr_token   tokens[static token_capacity],
                      const size_t             node_capacity,
                      struct spg_sexpr_node    nodes[static node_capacity],
                      struct spg_machine_goal *out) {
    if (out == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out = (struct spg_machine_goal){
        .max_temperature_mc     = SPG_GOAL_UNSET_S,
        .min_critical_health_bp = SPG_GOAL_UNSET,
        .max_actions            = SPG_GOAL_UNSET,
    };

    size_t                 token_count = 0u;
    size_t                 node_count  = 0u;
    struct spg_sexpr_error error       = {};
    const enum spg_status  status      = spg_sexpr_parse_text(
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
        !spg_sexpr_span_eq_cstr(input_n, input, nodes[head].span,
                                "machine-goal")) {
        return SPG_E_SCHEMA;
    }

    uint32_t node =
        field_value(input_n, input, nodes, root, "max-temperature-mc");
    if (node != SPG_SEXPR_INVALID_INDEX) {
        struct spg_text_span span   = nodes[node].span;
        bool                 negate = false;
        if (span.length > 0u && input[span.offset] == '-') {
            negate = true;
            span.offset += 1u;
            span.length -= 1u;
        }
        uint64_t magnitude = 0u;
        if (spg_sexpr_parse_uint64_span(input_n, input, span, &magnitude) !=
                SPG_OK ||
            magnitude > (uint64_t)INT64_MAX) {
            return SPG_E_SCHEMA;
        }
        out->max_temperature_mc =
            negate ? -(int64_t)magnitude : (int64_t)magnitude;
    }

    node = field_value(input_n, input, nodes, root,
                       "min-critical-service-health-bp");
    if (node != SPG_SEXPR_INVALID_INDEX) {
        if (spg_sexpr_parse_uint64_span(input_n, input, nodes[node].span,
                                        &out->min_critical_health_bp) !=
            SPG_OK) {
            return SPG_E_SCHEMA;
        }
        /* A share above 100% cannot be met by anything, which makes it a typo
         * rather than a very strict goal. */
        if (out->min_critical_health_bp > 10000u) {
            return SPG_E_SCHEMA;
        }
    }

    node = field_value(input_n, input, nodes, root, "max-actions");
    if (node != SPG_SEXPR_INVALID_INDEX &&
        spg_sexpr_parse_uint64_span(input_n, input, nodes[node].span,
                                    &out->max_actions) != SPG_OK) {
        return SPG_E_SCHEMA;
    }

    /* Note on (max-actions 0): it is NOT rejected as contradictory. A machine
     * that already meets its constraints satisfies the goal without acting,
     * and "verify without touching anything" is a legitimate run. What it does
     * mean is that any action at all fails the goal — which the evaluation
     * below reports as too_many_actions rather than as a parse error. */
    out->present = true;
    return SPG_OK;
}

/* Health of the critical services, in basis points: 10000 when every process
 * the profile marks critical is running, 0 when any is stopped or gone, and
 * UNKNOWN when none is named. Availability, not quality of service.
 *
 * static: the only caller is the evaluation below, and an export with no
 * outside consumer is API surface maintained for nobody. */
static uint64_t critical_health_bp(const struct spg_machine_state *state) {
    if (state == nullptr) {
        return SPG_MACHINE_UNKNOWN;
    }
    size_t critical  = 0u;
    size_t unhealthy = 0u;
    for (size_t i = 0u; i < state->n_processes; i += 1u) {
        const struct spg_process_sample *p = &state->processes[i];
        if (p->role != SPG_PROCESS_ROLE_CRITICAL) {
            continue;
        }
        critical += 1u;
        /* T is stopped, Z is a zombie. Either means the service is not serving,
         * whatever its CPU numbers say. */
        if (p->state == 'T' || p->state == 'Z') {
            unhealthy += 1u;
        }
    }
    if (critical == 0u) {
        /* Nothing declared critical: not perfect health, simply unmeasured.
         * Reporting 100% here would let a goal pass on a machine where the
         * service was never even identified. */
        return SPG_MACHINE_UNKNOWN;
    }
    return unhealthy == 0u ? 10000u : 0u;
}

enum spg_status
spg_machine_goal_evaluate(const struct spg_machine_goal  *goal,
                          const struct spg_machine_state *final_state,
                          const uint64_t                  actions_used,
                          struct spg_goal_evaluation     *out) {
    if (goal == nullptr || final_state == nullptr || out == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out = (struct spg_goal_evaluation){
        .verdict            = SPG_GOAL_SATISFIED,
        .temperature_mc     = final_state->temperature_mc,
        .critical_health_bp = critical_health_bp(final_state),
        .actions_used       = actions_used,
    };
    if (!goal->present) {
        out->verdict = SPG_GOAL_NO_GOAL;
        return SPG_OK;
    }

    /* Actions first: it is the only constraint that cannot be unmeasurable,
     * and a run that overspent has already failed regardless of the outcome. */
    if (goal->max_actions != SPG_GOAL_UNSET &&
        actions_used > goal->max_actions) {
        out->verdict = SPG_GOAL_TOO_MANY_ACTIONS;
        return SPG_OK;
    }

    if (goal->max_temperature_mc != SPG_GOAL_UNSET_S) {
        if (final_state->temperature_mc == SPG_MACHINE_UNKNOWN_S) {
            /* The constraint exists and the sensor does not. Conservative by
             * design: a limit nobody could measure was not respected, it was
             * merely not contradicted. */
            out->verdict = SPG_GOAL_UNMEASURABLE;
            return SPG_OK;
        }
        if (final_state->temperature_mc > goal->max_temperature_mc) {
            out->verdict = SPG_GOAL_TEMPERATURE_TOO_HIGH;
            return SPG_OK;
        }
    }

    if (goal->min_critical_health_bp != SPG_GOAL_UNSET) {
        if (out->critical_health_bp == SPG_MACHINE_UNKNOWN) {
            out->verdict = SPG_GOAL_UNMEASURABLE;
            return SPG_OK;
        }
        if (out->critical_health_bp < goal->min_critical_health_bp) {
            out->verdict = SPG_GOAL_CRITICAL_UNHEALTHY;
            return SPG_OK;
        }
    }
    return SPG_OK;
}

/* --- rendering ---------------------------------------------------------- */

struct writer {
    size_t capacity;
    char  *dst;
    size_t used;
    bool   overflowed;
};

static void put(struct writer *w, const char *text) {
    for (size_t i = 0u; text[i] != '\0'; i += 1u) {
        if (w->used + 1u < w->capacity) {
            w->dst[w->used] = text[i];
        } else {
            w->overflowed = true;
        }
        w->used += 1u;
    }
}

static void put_i64(struct writer *w, const int64_t value) {
    char     tmp[21];
    size_t   len = 0u;
    uint64_t mag = value < 0 ? (uint64_t)(-(value + 1)) + 1u : (uint64_t)value;
    do {
        tmp[len] = (char)('0' + (mag % 10u));
        len += 1u;
        mag /= 10u;
    } while (mag > 0u && len < sizeof tmp);
    if (value < 0) {
        put(w, "-");
    }
    char out[22];
    for (size_t i = 0u; i < len; i += 1u) {
        out[i] = tmp[len - 1u - i];
    }
    out[len] = '\0';
    put(w, out);
}

enum spg_status spg_machine_goal_render(const struct spg_machine_goal *goal,
                                        const size_t dst_capacity,
                                        char         dst[static dst_capacity],
                                        size_t      *out_required) {
    if (goal == nullptr || out_required == nullptr || dst_capacity == 0u) {
        return SPG_E_INVALID_ARG;
    }
    struct writer w = {.capacity = dst_capacity, .dst = dst};
    put(&w, "(machine-goal");
    if (goal->max_temperature_mc != SPG_GOAL_UNSET_S) {
        put(&w, " (max-temperature-mc ");
        put_i64(&w, goal->max_temperature_mc);
        put(&w, ")");
    }
    if (goal->min_critical_health_bp != SPG_GOAL_UNSET) {
        put(&w, " (min-critical-service-health-bp ");
        put_i64(&w, (int64_t)goal->min_critical_health_bp);
        put(&w, ")");
    }
    if (goal->max_actions != SPG_GOAL_UNSET) {
        put(&w, " (max-actions ");
        put_i64(&w, (int64_t)goal->max_actions);
        put(&w, ")");
    }
    put(&w, ")");

    *out_required = w.used + 1u;
    if (w.overflowed) {
        dst[0] = '\0';
        return SPG_E_LIMIT;
    }
    dst[w.used] = '\0';
    return SPG_OK;
}

const char *spg_goal_verdict_to_string(const enum spg_goal_verdict verdict) {
    switch (verdict) {
    case SPG_GOAL_SATISFIED:
        return "satisfied";
    case SPG_GOAL_TEMPERATURE_TOO_HIGH:
        return "temperature_too_high";
    case SPG_GOAL_CRITICAL_UNHEALTHY:
        return "critical_unhealthy";
    case SPG_GOAL_TOO_MANY_ACTIONS:
        return "too_many_actions";
    case SPG_GOAL_UNMEASURABLE:
        return "unmeasurable";
    case SPG_GOAL_NO_GOAL:
        return "no_goal";
    }
    return "unmeasurable";
}
