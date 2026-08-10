/* Phase 5: the rule baseline.
 *
 * The suspicious part of a baseline that scores 9/9 is that the same author
 * wrote the scenarios and the rules. Two things guard against fitting one to
 * the other, and both are here rather than in a promise:
 *
 *   1. The scenarios were committed in phase 4, before diagnose.c existed —
 *      check the history if you doubt it.
 *   2. The sensitivity sweep below moves every threshold by ±10% and reports
 *      how many answers survive. Rules tuned to specific numbers fall apart
 *      under that; rules that encode a judgement do not. */

#include "geistshell/diagnose.h"
#include "geistshell/machine_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct scenario {
    const char        *path;
    enum spg_diagnosis expected;
};

static const struct scenario scenarios[] = {
    {"examples/eval/machine/states/1_batch_cpu.spg",
     SPG_DIAGNOSIS_BATCH_PRESSURE},
    {"examples/eval/machine/states/2_critical_cpu.spg",
     SPG_DIAGNOSIS_CRITICAL_PRESSURE},
    {"examples/eval/machine/states/3_memory_batch.spg",
     SPG_DIAGNOSIS_MEMORY_PRESSURE},
    {"examples/eval/machine/states/4_thermal.spg",
     SPG_DIAGNOSIS_THERMAL_ANOMALY},
    {"examples/eval/machine/states/5_contradictory.spg",
     SPG_DIAGNOSIS_INCONCLUSIVE},
    {"examples/eval/machine/states/6_healthy.spg", SPG_DIAGNOSIS_HEALTHY},
    {"examples/eval/machine/states/7_heldout_thermal_batch.spg",
     SPG_DIAGNOSIS_THERMAL_ANOMALY},
    {"examples/eval/machine/states/8_heldout_memory_critical.spg",
     SPG_DIAGNOSIS_MEMORY_PRESSURE},
    {"examples/eval/machine/states/9_no_processes.spg",
     SPG_DIAGNOSIS_INCONCLUSIVE},
};

static const size_t scenario_count = sizeof scenarios / sizeof scenarios[0];

static bool load_state(const char *path, struct spg_machine_state *out) {
    FILE *f = fopen(path, "rb");
    if (f == nullptr) {
        printf("  cannot open %s (run from the repo root)\n", path);
        return false;
    }
    static char  text[8192];
    const size_t n = fread(text, 1u, sizeof text, f);
    (void)fclose(f);
    static struct spg_sexpr_token tokens[1024];
    static struct spg_sexpr_node  nodes[1024];
    return spg_machine_state_parse(n, text, 1024u, tokens, 1024u, nodes, out) ==
           SPG_OK;
}

static size_t score(const struct spg_rule_thresholds *th) {
    size_t correct = 0u;
    for (size_t i = 0u; i < scenario_count; i += 1u) {
        struct spg_machine_state state = {};
        if (!load_state(scenarios[i].path, &state)) {
            return SIZE_MAX;
        }
        struct spg_diagnosis_result r = {};
        if (spg_rule_diagnose(&state, th, &r) != SPG_OK) {
            return SIZE_MAX;
        }
        if (r.diagnosis == scenarios[i].expected) {
            correct += 1u;
        }
    }
    return correct;
}

static int test_default_thresholds(void) {
    const struct spg_rule_thresholds th  = spg_rule_thresholds_default();
    const size_t                     got = score(&th);
    if (got == SIZE_MAX) {
        return 1;
    }
    if (got != scenario_count) {
        printf("  rules scored %zu/%zu at the default thresholds\n", got,
               scenario_count);
        return 1;
    }
    return 0;
}

/* Scale every threshold at once, then each one alone. The counts are printed,
 * not just asserted: the number is the finding, and a test that hides it would
 * be the manipulation #65 warns against. */
static int test_threshold_sensitivity(void) {
    const struct spg_rule_thresholds base = spg_rule_thresholds_default();
    struct {
        const char *label;
        int         percent;
        bool        all;
        int         which; /* 0 cpu, 1 memory, 2 temperature, 3 share */
    } const sweeps[] = {
        {"all -10%", -10, true, 0},    {"all +10%", 10, true, 0},
        {"cpu -10%", -10, false, 0},   {"cpu +10%", 10, false, 0},
        {"mem -10%", -10, false, 1},   {"mem +10%", 10, false, 1},
        {"temp -10%", -10, false, 2},  {"temp +10%", 10, false, 2},
        {"share -10%", -10, false, 3}, {"share +10%", 10, false, 3},
    };
    size_t worst = scenario_count;
    for (size_t i = 0u; i < sizeof sweeps / sizeof sweeps[0]; i += 1u) {
        struct spg_rule_thresholds th = base;
        const int64_t              p  = sweeps[i].percent;
        if (sweeps[i].all || sweeps[i].which == 0) {
            th.cpu_high_bp = (uint64_t)((int64_t)base.cpu_high_bp +
                                        (int64_t)base.cpu_high_bp * p / 100);
        }
        if (sweeps[i].all || sweeps[i].which == 1) {
            th.memory_high_bp =
                (uint64_t)((int64_t)base.memory_high_bp +
                           (int64_t)base.memory_high_bp * p / 100);
        }
        if (sweeps[i].all || sweeps[i].which == 2) {
            th.temperature_high_mc =
                base.temperature_high_mc + base.temperature_high_mc * p / 100;
        }
        if (sweeps[i].all || sweeps[i].which == 3) {
            th.process_share_bp =
                (uint64_t)((int64_t)base.process_share_bp +
                           (int64_t)base.process_share_bp * p / 100);
        }
        const size_t got = score(&th);
        if (got == SIZE_MAX) {
            return 1;
        }
        printf("  sensitivity %-11s %zu/%zu\n", sweeps[i].label, got,
               scenario_count);
        if (got < worst) {
            worst = got;
        }
    }
    /* Measured, not chosen: 6/9 at "all +10%". The bar is set there so the
     * test catches a regression, and the number is printed so nobody has to
     * take this comment's word for it.
     *
     * What the sweep actually shows is a property of threshold rules, and it
     * is not flattering: they do not degrade, they flip. At +10% the memory
     * threshold sits at 99% and the 95%-used scenario stops being pressure;
     * the temperature threshold passes 82 C and a throttling board reads
     * `healthy`. Wrong-and-confident, not uncertain. Whether a model is more
     * robust to where the line sits is a real question — on this suite the one
     * we measured is not (see Diagnosis-Benchmark.md). */
    if (worst < 6u) {
        printf("  worst case under a 10%% shift: %zu/%zu\n", worst,
               scenario_count);
        return 1;
    }
    return 0;
}

static int test_abstains_rather_than_guesses(void) {
    const struct spg_rule_thresholds th = spg_rule_thresholds_default();
    struct spg_diagnosis_result      r  = {};

    /* Nothing readable at all. */
    struct spg_machine_state blind = {
        .cpu_utilisation_bp = SPG_MACHINE_UNKNOWN,
        .memory             = {.total_bytes = SPG_MACHINE_UNKNOWN,
                               .used_bytes  = SPG_MACHINE_UNKNOWN},
        .temperature_mc     = SPG_MACHINE_UNKNOWN_S,
    };
    if (spg_rule_diagnose(&blind, &th, &r) != SPG_OK ||
        r.diagnosis != SPG_DIAGNOSIS_INCONCLUSIVE) {
        return 1;
    }

    /* Saturated CPU, but the consumer is unmanaged: a real observation the
     * closed set cannot name, so abstain rather than pick a neighbour. */
    struct spg_machine_state anon = {
        .cpu_utilisation_bp = 9500u,
        .memory             = {.total_bytes = 1000u, .used_bytes = 100u},
        .temperature_mc     = 40000,
        .n_processes        = 1u,
    };
    anon.processes[0] = (struct spg_process_sample){
        .cpu_bp        = 9000u,
        .rss_bytes     = 10u,
        .role          = SPG_PROCESS_ROLE_UNKNOWN,
        .profile_index = SPG_PROCESS_NO_PROFILE,
    };
    if (spg_rule_diagnose(&anon, &th, &r) != SPG_OK ||
        r.diagnosis != SPG_DIAGNOSIS_INCONCLUSIVE) {
        return 1;
    }

    /* Two processes at 45% each: saturated, but blaming either would be
     * arbitrary. */
    struct spg_machine_state split = anon;
    split.n_processes              = 2u;
    split.processes[0].cpu_bp      = 4500u;
    split.processes[0].role        = SPG_PROCESS_ROLE_BATCH;
    split.processes[1]             = split.processes[0];
    split.processes[1].role        = SPG_PROCESS_ROLE_CRITICAL;
    if (spg_rule_diagnose(&split, &th, &r) != SPG_OK ||
        r.diagnosis != SPG_DIAGNOSIS_INCONCLUSIVE) {
        return 1;
    }
    return 0;
}

/* The same load with a different owner is a different problem. This is the one
 * case the process profile exists for. */
static int test_role_decides(void) {
    const struct spg_rule_thresholds th = spg_rule_thresholds_default();
    struct spg_machine_state         s  = {
        .cpu_utilisation_bp = 9400u,
        .memory             = {.total_bytes = 1000u, .used_bytes = 100u},
        .temperature_mc     = 40000,
        .n_processes        = 1u,
    };
    s.processes[0] = (struct spg_process_sample){
        .cpu_bp = 9000u, .role = SPG_PROCESS_ROLE_BATCH, .profile_index = 0u};
    memcpy(s.processes[0].profile_id, "b", sizeof "b");
    struct spg_diagnosis_result r = {};
    if (spg_rule_diagnose(&s, &th, &r) != SPG_OK ||
        r.diagnosis != SPG_DIAGNOSIS_BATCH_PRESSURE ||
        strcmp(r.process_id, "b") != 0) {
        return 1;
    }
    s.processes[0].role = SPG_PROCESS_ROLE_CRITICAL;
    if (spg_rule_diagnose(&s, &th, &r) != SPG_OK ||
        r.diagnosis != SPG_DIAGNOSIS_CRITICAL_PRESSURE) {
        return 1;
    }
    return 0;
}

static int test_boundaries(void) {
    const struct spg_rule_thresholds th = spg_rule_thresholds_default();
    struct spg_diagnosis_result      r  = {};
    /* Exactly at the temperature threshold is NOT above it. */
    struct spg_machine_state s = {
        .cpu_utilisation_bp = 100u,
        .memory             = {.total_bytes = 1000u, .used_bytes = 100u},
        .temperature_mc     = th.temperature_high_mc,
    };
    if (spg_rule_diagnose(&s, &th, &r) != SPG_OK ||
        r.diagnosis != SPG_DIAGNOSIS_HEALTHY) {
        return 1;
    }
    s.temperature_mc = th.temperature_high_mc + 1;
    if (spg_rule_diagnose(&s, &th, &r) != SPG_OK ||
        r.diagnosis != SPG_DIAGNOSIS_THERMAL_ANOMALY) {
        return 1;
    }
    /* used > total is nonsense a kernel can still report: clamp, do not wrap
     * into a huge percentage. */
    s.temperature_mc     = 40000;
    s.memory.used_bytes  = 5000u;
    s.memory.total_bytes = 1000u;
    if (spg_rule_diagnose(&s, &th, &r) != SPG_OK ||
        r.diagnosis != SPG_DIAGNOSIS_MEMORY_PRESSURE) {
        return 1;
    }
    /* A zero-sized machine cannot divide. */
    s.memory.total_bytes = 0u;
    if (spg_rule_diagnose(&s, &th, &r) != SPG_OK) {
        return 1;
    }
    return 0;
}

static int test_null_args(void) {
    const struct spg_rule_thresholds th = spg_rule_thresholds_default();
    struct spg_machine_state         s  = {};
    struct spg_diagnosis_result      r  = {};
    if (spg_rule_diagnose(nullptr, &th, &r) != SPG_E_INVALID_ARG ||
        spg_rule_diagnose(&s, nullptr, &r) != SPG_E_INVALID_ARG ||
        spg_rule_diagnose(&s, &th, nullptr) != SPG_E_INVALID_ARG) {
        return 1;
    }
    bool ok = true;
    (void)spg_diagnosis_from_string("not_a_category", &ok);
    if (ok) {
        return 1;
    }
    (void)spg_diagnosis_from_string(nullptr, &ok);
    if (ok) {
        return 1;
    }
    if (!ok && spg_diagnosis_from_string("thermal_anomaly", &ok) !=
                   SPG_DIAGNOSIS_THERMAL_ANOMALY) {
        return 1;
    }
    return ok ? 0 : 1;
}

int main(void) {
    const struct {
        const char *name;
        int (*fn)(void);
    } cases[] = {
        {"default_thresholds", test_default_thresholds},
        {"threshold_sensitivity", test_threshold_sensitivity},
        {"abstains_rather_than_guesses", test_abstains_rather_than_guesses},
        {"role_decides", test_role_decides},
        {"boundaries", test_boundaries},
        {"null_args", test_null_args},
    };
    int failures = 0;
    for (size_t i = 0u; i < sizeof cases / sizeof cases[0]; i += 1u) {
        if (cases[i].fn() != 0) {
            printf("test_machine_diagnose: FAIL %s\n", cases[i].name);
            failures += 1;
        }
    }
    if (failures == 0) {
        printf("test_machine_diagnose: PASS\n");
    }
    return failures == 0 ? 0 : 1;
}
