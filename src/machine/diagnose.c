/* The rule baseline. Four rules, in a fixed priority, over the same snapshot
 * the model sees. Nothing here knows about eval cases, process names or the
 * benchmark — it reads fields and thresholds. */

#include "geistshell/diagnose.h"

#include <string.h>

struct spg_rule_thresholds spg_rule_thresholds_default(void) {
    return (struct spg_rule_thresholds){
        /* 80% system CPU. Below that a machine is busy, not saturated. */
        .cpu_high_bp = 8000u,
        /* 90% of memory in use. Linux happily runs at 70% forever. */
        .memory_high_bp = 9000u,
        /* 75 C. A Pi throttles around 80; this is the warning shoulder. */
        .temperature_high_mc = 75000,
        /* A process must own half the machine's CPU before the saturation is
         * called its fault. Two processes at 45% each is a different problem
         * from one at 90%, and blaming either would be arbitrary. */
        .process_share_bp = 5000u,
    };
}

static bool known(const uint64_t v) { return v != SPG_MACHINE_UNKNOWN; }

/* The busiest process, or null when the list is empty or nothing has a
 * measurable share. */
static const struct spg_process_sample *
hottest_cpu(const struct spg_machine_state *state) {
    const struct spg_process_sample *best = nullptr;
    for (size_t i = 0u; i < state->n_processes; i += 1u) {
        const struct spg_process_sample *p = &state->processes[i];
        if (!known(p->cpu_bp)) {
            continue;
        }
        if (best == nullptr || p->cpu_bp > best->cpu_bp) {
            best = p;
        }
    }
    return best;
}

static const struct spg_process_sample *
largest_rss(const struct spg_machine_state *state) {
    const struct spg_process_sample *best = nullptr;
    for (size_t i = 0u; i < state->n_processes; i += 1u) {
        const struct spg_process_sample *p = &state->processes[i];
        if (!known(p->rss_bytes)) {
            continue;
        }
        if (best == nullptr || p->rss_bytes > best->rss_bytes) {
            best = p;
        }
    }
    return best;
}

static void attribute(struct spg_diagnosis_result     *out,
                      const struct spg_process_sample *p) {
    if (p != nullptr && p->profile_id[0] != '\0') {
        memcpy(out->process_id, p->profile_id, SPG_PROCESS_ID_CAP);
        out->process_id[SPG_PROCESS_ID_CAP - 1u] = '\0';
    }
}

enum spg_status spg_rule_diagnose(const struct spg_machine_state   *state,
                                  const struct spg_rule_thresholds *thresholds,
                                  struct spg_diagnosis_result      *out) {
    if (state == nullptr || thresholds == nullptr || out == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out =
        (struct spg_diagnosis_result){.diagnosis = SPG_DIAGNOSIS_INCONCLUSIVE};

    /* Rule 1 — heat first. A thermal fault is the one cause no process change
     * fixes: pausing the busiest job does not repair a fan. Ordering it ahead
     * of load is a judgement about consequences, not about the test data. */
    if (state->temperature_mc != SPG_MACHINE_UNKNOWN_S &&
        state->temperature_mc > thresholds->temperature_high_mc) {
        out->diagnosis = SPG_DIAGNOSIS_THERMAL_ANOMALY;
        return SPG_OK;
    }

    /* Rule 2 — memory before CPU. Memory pressure ends in the OOM killer;
     * CPU pressure ends in things being slow. */
    if (known(state->memory.total_bytes) && state->memory.total_bytes > 0u &&
        known(state->memory.used_bytes)) {
        const uint64_t used_bp =
            state->memory.used_bytes > state->memory.total_bytes
                ? 10000u
                : state->memory.used_bytes * 10000u / state->memory.total_bytes;
        if (used_bp >= thresholds->memory_high_bp) {
            out->diagnosis = SPG_DIAGNOSIS_MEMORY_PRESSURE;
            attribute(out, largest_rss(state));
            return SPG_OK;
        }
    }

    /* Rule 3 — saturated CPU, attributed by ROLE, not by name. This is the
     * whole point of the process profile: the same load is a different problem
     * depending on who is causing it. */
    if (known(state->cpu_utilisation_bp) &&
        state->cpu_utilisation_bp >= thresholds->cpu_high_bp) {
        const struct spg_process_sample *hot = hottest_cpu(state);
        if (hot == nullptr || hot->cpu_bp < thresholds->process_share_bp) {
            /* Saturated, but nobody owns enough of it to be blamed. Naming a
             * process anyway would be the hallucination this baseline exists
             * to avoid. */
            out->diagnosis = SPG_DIAGNOSIS_INCONCLUSIVE;
            return SPG_OK;
        }
        switch (hot->role) {
        case SPG_PROCESS_ROLE_BATCH:
            out->diagnosis = SPG_DIAGNOSIS_BATCH_PRESSURE;
            break;
        case SPG_PROCESS_ROLE_CRITICAL:
            out->diagnosis = SPG_DIAGNOSIS_CRITICAL_PRESSURE;
            break;
        case SPG_PROCESS_ROLE_UNKNOWN:
            /* An unmanaged process is consuming the machine. That is a real
             * observation, but not one of the causes this closed set can name.
             */
            out->diagnosis = SPG_DIAGNOSIS_INCONCLUSIVE;
            break;
        }
        attribute(out, hot);
        return SPG_OK;
    }

    /* Rule 4 — nothing above threshold. Only call it healthy if the signals
     * that would have shown a problem were actually readable; otherwise the
     * snapshot is too thin to conclude anything. */
    const bool can_see_load = known(state->cpu_utilisation_bp);
    const bool can_see_memory =
        known(state->memory.total_bytes) && known(state->memory.used_bytes);
    const bool can_see_heat = state->temperature_mc != SPG_MACHINE_UNKNOWN_S;
    if (can_see_load && can_see_memory && can_see_heat) {
        out->diagnosis = SPG_DIAGNOSIS_HEALTHY;
    }
    return SPG_OK;
}

const char *spg_diagnosis_to_string(const enum spg_diagnosis d) {
    switch (d) {
    case SPG_DIAGNOSIS_INCONCLUSIVE:
        return "inconclusive";
    case SPG_DIAGNOSIS_HEALTHY:
        return "healthy";
    case SPG_DIAGNOSIS_BATCH_PRESSURE:
        return "batch_pressure";
    case SPG_DIAGNOSIS_CRITICAL_PRESSURE:
        return "critical_pressure";
    case SPG_DIAGNOSIS_MEMORY_PRESSURE:
        return "memory_pressure";
    case SPG_DIAGNOSIS_THERMAL_ANOMALY:
        return "thermal_anomaly";
    }
    return "inconclusive";
}

enum spg_diagnosis spg_diagnosis_from_string(const char *text, bool *ok) {
    static const enum spg_diagnosis all[] = {
        SPG_DIAGNOSIS_INCONCLUSIVE,    SPG_DIAGNOSIS_HEALTHY,
        SPG_DIAGNOSIS_BATCH_PRESSURE,  SPG_DIAGNOSIS_CRITICAL_PRESSURE,
        SPG_DIAGNOSIS_MEMORY_PRESSURE, SPG_DIAGNOSIS_THERMAL_ANOMALY,
    };
    if (ok != nullptr) {
        *ok = false;
    }
    if (text == nullptr) {
        return SPG_DIAGNOSIS_INCONCLUSIVE;
    }
    for (size_t i = 0u; i < sizeof all / sizeof all[0]; i += 1u) {
        if (strcmp(spg_diagnosis_to_string(all[i]), text) == 0) {
            if (ok != nullptr) {
                *ok = true;
            }
            return all[i];
        }
    }
    return SPG_DIAGNOSIS_INCONCLUSIVE;
}
