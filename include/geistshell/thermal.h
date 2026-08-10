#ifndef GEISTSHELL_THERMAL_H
#define GEISTSHELL_THERMAL_H

#include "geistshell/status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Machine B: a thermal loop with a real actuator (roadmap phase 15, #75).
 *
 * Machine A's actions pause processes — reversible, discrete, and the world
 * only responds through the scheduler. Machine B turns a fan, and the world
 * responds through physics: slowly, continuously, and with a lag the agent has
 * to live with. That difference is the point of the phase; the question is how
 * much of geistshell had to change to carry it.
 *
 * Hardware: the Pi 5's pwmfan hwmon device (pwm1 0..255, fan1_input RPM) and
 * the cpu_thermal zone. No heating element — CPU load is the heater, and the
 * workloads from #68 already produce it. */

/* Calibration is configuration, never constants.
 *
 * A real fan does not follow its duty cycle linearly, a real sensor reads a
 * degree or two off, and no two boards agree. Numbers baked into the code
 * would be right for exactly one machine — which is the failure mode this
 * whole phase is testing for. */
struct spg_thermal_calibration {
    int64_t  sensor_offset_mc; /* added to every reading */
    uint64_t min_duty;         /* below this the fan stalls rather than
                                * spinning slowly */
    uint64_t max_duty;
    /* Never below this, whatever the agent asks. A fan the model can switch
     * off entirely is a way to cook the board with one allowed action. */
    uint64_t floor_duty;
    /* Minimum ticks between two changes. Physics is slow; an agent that
     * re-decides every second oscillates and measures its own noise. */
    uint64_t min_interval_ticks;
};

[[nodiscard]] struct spg_thermal_calibration
spg_thermal_calibration_default(void);

struct spg_thermal_state {
    int64_t  temperature_mc; /* calibrated; UNKNOWN_S when unreadable */
    uint64_t fan_rpm;        /* measured, SPG_MACHINE_UNKNOWN when absent */
    uint64_t fan_duty;       /* 0..255 as the driver reports it */
    bool     present;        /* false: no fan device on this host */
};

/* Read the thermal channel. Returns SPG_E_UNSUPPORTED where there is no fan;
 * *out is then zeroed with present=false, and a caller must keep running — a
 * machine without a fan is a machine, not an error. */
[[nodiscard]] enum spg_status
spg_thermal_read(const struct spg_thermal_calibration *calibration,
                 struct spg_thermal_state             *out);

/* Set the fan duty, clamped into [floor_duty, max_duty] and to at least
 * min_duty when non-zero. Returns what was actually written in *out_applied,
 * because the number the agent asked for and the number the hardware got are
 * different things and the journal must record the second. */
[[nodiscard]] enum spg_status
spg_thermal_set_duty(const struct spg_thermal_calibration *calibration,
                     uint64_t requested, uint64_t *out_applied);

/* Parse "fan:60" into a duty of 60 percent expressed as 0..255.
 *
 * The level rides inside the existing `target` string rather than in a new
 * grammar field. That is a deliberate compromise and a finding of this phase:
 * adding a parameterised action to a second machine without touching the
 * recommendation grammar means encoding the parameter into a string. See
 * docs/machine-intelligence/Machine-B.md. */
[[nodiscard]] bool spg_thermal_parse_target(const char *target,
                                            uint64_t   *out_duty);

#ifdef __cplusplus
}
#endif

#endif
