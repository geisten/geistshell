/* Machine B (#75). The parts that can be tested without a fan: calibration,
 * clamping, and the target encoding. The hardware itself is exercised on the
 * Pi, where there is one. */

#include "geistshell/thermal.h"

#include <stdio.h>
#include <string.h>

static int test_target_encoding(void) {
    uint64_t duty = 0u;
    if (!spg_thermal_parse_target("fan:100", &duty) || duty != 255u) {
        return 1;
    }
    if (!spg_thermal_parse_target("fan:0", &duty) || duty != 0u) {
        return 1;
    }
    if (!spg_thermal_parse_target("fan:60", &duty) || duty != 153u) {
        return 1;
    }
    /* Everything else is refused rather than interpreted: a target the agent
     * meant differently must not become a fan setting by accident. */
    const char *bad[] = {"fan:101",   "fan:60rpm", "fan:",  "fan",
                         "batch_job", "",          "fan:-1"};
    for (size_t i = 0u; i < sizeof bad / sizeof bad[0]; i += 1u) {
        if (spg_thermal_parse_target(bad[i], &duty)) {
            printf("  accepted %s\n", bad[i]);
            return 1;
        }
    }
    return spg_thermal_parse_target(nullptr, &duty) ? 1 : 0;
}

/* The agent may slow the fan. It may not stop it: switching the fan off is one
 * allowed action away from cooking the board, and no goal is worth that. */
static int test_floor_holds(void) {
    const struct spg_thermal_calibration c = spg_thermal_calibration_default();
    uint64_t                             applied = 999u;
    (void)spg_thermal_set_duty(&c, 0u, &applied);
    if (applied < c.floor_duty) {
        printf("  the fan was allowed to stop: %llu\n",
               (unsigned long long)applied);
        return 1;
    }
    (void)spg_thermal_set_duty(&c, 10000u, &applied);
    if (applied > c.max_duty) {
        return 1;
    }
    /* Between floor and min_duty the fan hums without moving air, so a
     * non-zero request below min_duty is raised rather than honoured. */
    (void)spg_thermal_set_duty(&c, c.floor_duty + 1u, &applied);
    if (applied != 0u && applied < c.min_duty) {
        return 1;
    }
    return 0;
}

/* Calibration is configuration. Numbers baked into the code would be right for
 * exactly one board — the failure mode this whole phase tests for. */
static int test_calibration_is_data(void) {
    struct spg_thermal_calibration c = spg_thermal_calibration_default();
    if (c.max_duty <= c.floor_duty || c.min_interval_ticks == 0u) {
        return 1;
    }
    c.floor_duty     = 200u;
    c.max_duty       = 255u;
    uint64_t applied = 0u;
    (void)spg_thermal_set_duty(&c, 10u, &applied);
    return applied == 200u ? 0 : 1;
}

static int test_degrades_without_hardware(void) {
    const struct spg_thermal_calibration c = spg_thermal_calibration_default();
    struct spg_thermal_state             s = {};
    const enum spg_status                status = spg_thermal_read(&c, &s);
    /* A machine without a fan is a machine, not an error: the caller keeps
     * running and the field reads unknown. */
    if (status == SPG_OK && !s.present) {
        return 1;
    }
    if (status != SPG_OK && s.present) {
        return 1;
    }
    if (spg_thermal_read(&c, nullptr) != SPG_E_INVALID_ARG) {
        return 1;
    }
    return 0;
}

int main(void) {
    const struct {
        const char *name;
        int (*fn)(void);
    } cases[] = {
        {"target_encoding", test_target_encoding},
        {"floor_holds", test_floor_holds},
        {"calibration_is_data", test_calibration_is_data},
        {"degrades_without_hardware", test_degrades_without_hardware},
    };
    int failures = 0;
    for (size_t i = 0u; i < sizeof cases / sizeof cases[0]; i += 1u) {
        if (cases[i].fn() != 0) {
            printf("test_machine_thermal: FAIL %s\n", cases[i].name);
            failures += 1;
        }
    }
    if (failures == 0) {
        printf("test_machine_thermal: PASS\n");
    }
    return failures == 0 ? 0 : 1;
}
