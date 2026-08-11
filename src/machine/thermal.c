/* Machine B: the fan.
 *
 * What lives here is the decision layer — calibration and the clamp. Reading
 * and writing go through the port boundary, so a platform port cannot
 * accidentally change what the agent is allowed to do to a fan. */

#include "geistshell/thermal.h"

#include "geistshell/machine_backend.h"
#include "geistshell/machine_state.h"

#include <string.h>

struct spg_thermal_calibration spg_thermal_calibration_default(void) {
    return (struct spg_thermal_calibration){
        .sensor_offset_mc = 0,
        /* Below roughly a fifth of full duty a small fan hums without moving
         * air; the value belongs to the fan, not to this code, which is why it
         * is here and not in an if. */
        .min_duty = 60u,
        .max_duty = 255u,
        /* The agent may slow the fan, never stop it. Switching it off is one
         * allowed action away from cooking the board, and no goal is worth
         * that. */
        .floor_duty         = 40u,
        .min_interval_ticks = 2u,
    };
}

/* Clamping is a safety decision, not an I/O detail, so it sits above the port
 * boundary. An earlier version had it inside the Linux branch, where the
 * property silently did not exist anywhere else and could not be tested on the
 * machine it was written on — a test caught that, and the port interface exists
 * partly to make the mistake unrepeatable. */
static uint64_t clamp_duty(const struct spg_thermal_calibration *calibration,
                           const uint64_t                        requested) {
    uint64_t duty = requested;
    if (duty > calibration->max_duty) {
        duty = calibration->max_duty;
    }
    if (duty < calibration->floor_duty) {
        /* Clamped, not refused: less air is a legitimate request. Off is
         * not. */
        duty = calibration->floor_duty;
    }
    if (duty > 0u && duty < calibration->min_duty) {
        duty = calibration->min_duty; /* a stalled fan moves no air at all */
    }
    return duty;
}

enum spg_status
spg_thermal_read(const struct spg_thermal_calibration *calibration,
                 struct spg_thermal_state             *out) {
    if (calibration == nullptr || out == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out = (struct spg_thermal_state){.temperature_mc = SPG_MACHINE_UNKNOWN_S,
                                      .fan_rpm        = SPG_MACHINE_UNKNOWN,
                                      .fan_duty       = SPG_MACHINE_UNKNOWN};

    int64_t milli = 0;
    if (spg_backend_temperature(&milli) == SPG_OK) {
        /* Calibrated, not raw: the offset is what makes one board's reading
         * comparable to another's. */
        out->temperature_mc = milli + calibration->sensor_offset_mc;
    }
    if (spg_backend_fan_read(&out->fan_rpm, &out->fan_duty) != SPG_OK) {
        return SPG_E_UNSUPPORTED;
    }
    out->present = true;
    return SPG_OK;
}

enum spg_status
spg_thermal_set_duty(const struct spg_thermal_calibration *calibration,
                     const uint64_t requested, uint64_t *out_applied) {
    if (calibration == nullptr || out_applied == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    /* Clamped before the backend sees it, and reported whatever the backend
     * then says: the number a caller gets back must not depend on which host
     * it asked, and the journal records what the hardware got rather than what
     * the agent wanted. */
    const uint64_t duty = clamp_duty(calibration, requested);
    *out_applied        = duty;
    return spg_backend_fan_write(duty);
}

bool spg_thermal_parse_target(const char *target, uint64_t *out_duty) {
    if (target == nullptr || out_duty == nullptr) {
        return false;
    }
    if (strncmp(target, "fan:", 4u) != 0) {
        return false;
    }
    const char *p       = target + 4u;
    uint64_t    percent = 0u;
    bool        any     = false;
    for (; *p >= '0' && *p <= '9'; p += 1) {
        percent = percent * 10u + (uint64_t)(*p - '0');
        if (percent > 100u) {
            return false;
        }
        any = true;
    }
    if (!any || *p != '\0') {
        return false; /* "fan:60rpm" is not a percentage */
    }
    *out_duty = percent * 255u / 100u;
    return true;
}
