/* Machine B: the fan. Real hardware behind a hwmon file, or nothing at all. */

#define _POSIX_C_SOURCE 200809L

#include "geistshell/thermal.h"

#include "geistshell/machine_state.h"

#include <stdio.h>
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

/* Clamping lives OUTSIDE the platform branch on purpose.
 *
 * It is a safety decision, not an I/O detail: the fan floor is what stops one
 * allowed action from cooking the board. Behind #if defined(__linux__) the
 * property would silently not exist anywhere else — and, worse, could not be
 * tested on a development machine at all. My own test caught exactly that. */
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

#if defined(__linux__)

/* The pwmfan hwmon device, found by name rather than by a fixed hwmonN path:
 * the numbering depends on probe order and changes across boots. */
static bool fan_dir(char out[static 64]) {
    for (unsigned i = 0u; i < 16u; i += 1u) {
        char path[96];
        if (snprintf(path, sizeof path, "/sys/class/hwmon/hwmon%u/name", i) <
            0) {
            continue;
        }
        FILE *f = fopen(path, "rbe");
        if (f == nullptr) {
            continue;
        }
        char         name[32] = {};
        const size_t n        = fread(name, 1u, sizeof name - 1u, f);
        (void)fclose(f);
        if (n >= 6u && memcmp(name, "pwmfan", 6u) == 0) {
            return snprintf(out, 64u, "/sys/class/hwmon/hwmon%u", i) > 0;
        }
    }
    return false;
}

static bool read_u64_file(const char *path, uint64_t *out) {
    FILE *f = fopen(path, "rbe");
    if (f == nullptr) {
        return false;
    }
    char         buf[32] = {};
    const size_t n       = fread(buf, 1u, sizeof buf - 1u, f);
    (void)fclose(f);
    if (n == 0u) {
        return false;
    }
    uint64_t value = 0u;
    bool     any   = false;
    for (size_t i = 0u; i < n && buf[i] >= '0' && buf[i] <= '9'; i += 1u) {
        value = value * 10u + (uint64_t)(buf[i] - '0');
        any   = true;
    }
    if (any) {
        *out = value;
    }
    return any;
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

    uint64_t milli = 0u;
    if (read_u64_file("/sys/class/thermal/thermal_zone0/temp", &milli)) {
        /* Calibrated, not raw: the offset is what makes one board's reading
         * comparable to another's. */
        out->temperature_mc = (int64_t)milli + calibration->sensor_offset_mc;
    }

    char dir[64];
    if (!fan_dir(dir)) {
        return SPG_E_UNSUPPORTED;
    }
    char path[128];
    if (snprintf(path, sizeof path, "%s/fan1_input", dir) > 0) {
        (void)read_u64_file(path, &out->fan_rpm);
    }
    if (snprintf(path, sizeof path, "%s/pwm1", dir) > 0) {
        (void)read_u64_file(path, &out->fan_duty);
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
    const uint64_t duty = clamp_duty(calibration, requested);
    *out_applied        = duty;

    char dir[64];
    if (!fan_dir(dir)) {
        return SPG_E_UNSUPPORTED;
    }
    char path[128];
    if (snprintf(path, sizeof path, "%s/pwm1", dir) < 0) {
        return SPG_E_IO;
    }
    FILE *f = fopen(path, "wbe");
    if (f == nullptr) {
        return SPG_E_IO; /* writing pwm1 usually needs root */
    }
    const int written = fprintf(f, "%llu\n", (unsigned long long)duty);
    (void)fclose(f);
    return written > 0 ? SPG_OK : SPG_E_IO;
}

#else

enum spg_status
spg_thermal_read(const struct spg_thermal_calibration *calibration,
                 struct spg_thermal_state             *out) {
    (void)calibration;
    if (out == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out = (struct spg_thermal_state){.temperature_mc = SPG_MACHINE_UNKNOWN_S,
                                      .fan_rpm        = SPG_MACHINE_UNKNOWN,
                                      .fan_duty       = SPG_MACHINE_UNKNOWN};
    return SPG_E_UNSUPPORTED;
}

enum spg_status
spg_thermal_set_duty(const struct spg_thermal_calibration *calibration,
                     const uint64_t requested, uint64_t *out_applied) {
    if (calibration == nullptr || out_applied == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    /* Same clamp as on the machine that has a fan: the number a caller gets
     * back must not depend on which host it asked. */
    *out_applied = clamp_duty(calibration, requested);
    return SPG_E_UNSUPPORTED;
}

#endif

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
