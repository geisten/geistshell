/* Layer 1: the only part that touches the OS. Kept separate so the parsers
 * stay testable on any host with static fixtures — the tests never read /proc.
 *
 * Linux only. Elsewhere the sampler reports SPG_E_UNSUPPORTED and every field
 * stays unknown, so a caller on a developer Mac keeps running. */

#define _POSIX_C_SOURCE 200809L

#include "geistshell/machine_state.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

static void state_init(struct spg_machine_state *out,
                       const uint64_t            timestamp_ns) {
    *out = (struct spg_machine_state){
        .timestamp_ns       = timestamp_ns,
        .cpu_utilisation_bp = SPG_MACHINE_UNKNOWN,
        .load =
            {
                .avg_1_cbp  = SPG_MACHINE_UNKNOWN,
                .avg_5_cbp  = SPG_MACHINE_UNKNOWN,
                .avg_15_cbp = SPG_MACHINE_UNKNOWN,
            },
        .memory =
            {
                .total_bytes     = SPG_MACHINE_UNKNOWN,
                .used_bytes      = SPG_MACHINE_UNKNOWN,
                .swap_used_bytes = SPG_MACHINE_UNKNOWN,
            },
        .temperature_mc = SPG_MACHINE_UNKNOWN_S,
        .cpu_freq_khz   = SPG_MACHINE_UNKNOWN,
        .throttle       = SPG_THROTTLE_UNKNOWN,
        .process_count  = SPG_MACHINE_UNKNOWN,
    };
}

#if defined(__linux__)

#    include <dirent.h>

/* Caller-provided buffer, no allocation. Returns the byte count read, or 0 when
 * the file is missing — a missing sysfs file is normal, not an error. */
static size_t read_file(const char *path, const size_t cap, char buf[cap]) {
    FILE *f = fopen(path, "rbe");
    if (f == nullptr) {
        return 0u;
    }
    const size_t n = fread(buf, 1u, cap, f);
    (void)fclose(f);
    return n;
}

/* /proc entries whose name is all digits are processes. */
static uint64_t count_processes(void) {
    DIR *dir = opendir("/proc");
    if (dir == nullptr) {
        return SPG_MACHINE_UNKNOWN;
    }
    uint64_t             count = 0u;
    const struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        bool numeric = entry->d_name[0] != '\0';
        for (size_t i = 0u; numeric && entry->d_name[i] != '\0'; i += 1u) {
            numeric = entry->d_name[i] >= '0' && entry->d_name[i] <= '9';
        }
        if (numeric) {
            count += 1u;
        }
    }
    (void)closedir(dir);
    return count;
}

enum spg_status spg_machine_sample(const uint64_t               timestamp_ns,
                                   const struct spg_cpu_sample *prev,
                                   struct spg_machine_state    *out) {
    if (out == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    state_init(out, timestamp_ns);

    /* One buffer, reused: /proc/meminfo is the largest of these by far. */
    char   buf[4096];
    size_t n = read_file("/proc/stat", sizeof buf, buf);
    if (n > 0u && spg_telemetry_parse_stat(n, buf, &out->cpu) == SPG_OK) {
        out->cpu_utilisation_bp = spg_telemetry_utilisation_bp(prev, &out->cpu);
    }
    n = read_file("/proc/meminfo", sizeof buf, buf);
    if (n > 0u) {
        (void)spg_telemetry_parse_meminfo(n, buf, &out->memory);
    }
    n = read_file("/proc/loadavg", sizeof buf, buf);
    if (n > 0u) {
        (void)spg_telemetry_parse_loadavg(n, buf, &out->load);
    }

    /* thermal_zone0 is the CPU zone on the Pi and on most ARM boards. A host
     * without it simply reports unknown. */
    n = read_file("/sys/class/thermal/thermal_zone0/temp", sizeof buf, buf);
    if (n > 0u) {
        int64_t milli = 0;
        if (spg_telemetry_parse_int(n, buf, &milli) == SPG_OK) {
            out->temperature_mc = milli;
        }
    }
    n = read_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq",
                  sizeof buf, buf);
    if (n > 0u) {
        int64_t khz = 0;
        if (spg_telemetry_parse_int(n, buf, &khz) == SPG_OK && khz >= 0) {
            out->cpu_freq_khz = (uint64_t)khz;
        }
    }
    /* Pi-specific and optional by design: no vcgencmd dependency. */
    n = read_file("/sys/devices/platform/soc/soc:firmware/get_throttled",
                  sizeof buf, buf);
    if (n > 0u) {
        out->throttle = spg_telemetry_parse_throttle(n, buf);
    }
    out->process_count = count_processes();
    return SPG_OK;
}

#else /* not Linux */

enum spg_status spg_machine_sample(const uint64_t               timestamp_ns,
                                   const struct spg_cpu_sample *prev,
                                   struct spg_machine_state    *out) {
    (void)prev;
    if (out == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    state_init(out, timestamp_ns);
    return SPG_E_UNSUPPORTED;
}

#endif
