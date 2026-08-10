#ifndef GEISTSHELL_DIAGNOSE_H
#define GEISTSHELL_DIAGNOSE_H

#include "geistshell/machine_state.h"
#include "geistshell/status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Deliberately simple rule baseline for machine diagnosis (roadmap phase 5,
 * #65).
 *
 * It exists to make the central hypothesis falsifiable. Claiming a model helps
 * requires something for it to be better than, and "better than nothing" is not
 * a claim worth making. If the models do not beat this, that is the result —
 * see docs/machine-intelligence/Diagnosis-Benchmark.md.
 *
 * Pure: no I/O, no model, no allocation, no clock. */

/* The closed set of root causes, shared by the rules and the eval harness so
 * there is one definition rather than two that can drift.
 * INCONCLUSIVE is abstention: the honest answer when the evidence does not
 * support any cause. It is counted separately from a wrong answer. */
enum spg_diagnosis {
    SPG_DIAGNOSIS_INCONCLUSIVE = 0,
    SPG_DIAGNOSIS_HEALTHY,
    SPG_DIAGNOSIS_BATCH_PRESSURE,
    SPG_DIAGNOSIS_CRITICAL_PRESSURE,
    SPG_DIAGNOSIS_MEMORY_PRESSURE,
    SPG_DIAGNOSIS_THERMAL_ANOMALY,
};

/* Thresholds are data, not constants baked into the branches. Two reasons:
 * a reviewer can see every number in one place, and the sensitivity sweep
 * (#65: "do the rules still make sense at ±10%?") can run without a rebuild —
 * a criterion nobody can execute is a criterion nobody checks. */
struct spg_rule_thresholds {
    uint64_t cpu_high_bp;    /* system CPU that counts as saturated */
    uint64_t memory_high_bp; /* used/total share that counts as pressure */
    int64_t  temperature_high_mc;
    uint64_t process_share_bp; /* a process must own this much of the CPU
                                * before the load is attributed to it */
};

[[nodiscard]] struct spg_rule_thresholds spg_rule_thresholds_default(void);

struct spg_diagnosis_result {
    enum spg_diagnosis diagnosis;
    /* The process the diagnosis is attributed to, or "" when the cause is not
     * a process (thermal, healthy) or cannot be attributed. */
    char process_id[SPG_PROCESS_ID_CAP];
};

/* Apply the rules. Returns SPG_E_INVALID_ARG on a null argument; every other
 * input yields a result, and an input the rules cannot explain yields
 * INCONCLUSIVE rather than a guess. */
[[nodiscard]] enum spg_status
spg_rule_diagnose(const struct spg_machine_state   *state,
                  const struct spg_rule_thresholds *thresholds,
                  struct spg_diagnosis_result      *out);

[[nodiscard]] const char *spg_diagnosis_to_string(enum spg_diagnosis d);

/* Parse a category token back to the enum; SPG_DIAGNOSIS_INCONCLUSIVE with
 * *ok=false for anything outside the closed set. */
[[nodiscard]] enum spg_diagnosis spg_diagnosis_from_string(const char *text,
                                                           bool       *ok);

#ifdef __cplusplus
}
#endif

#endif
