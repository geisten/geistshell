/* Layer 1: the only part that touches the OS. Kept separate so the parsers
 * stay testable on any host with static fixtures — the tests never read /proc.
 *
 * Linux only. Elsewhere the sampler reports SPG_E_UNSUPPORTED and every field
 * stays unknown, so a caller on a developer Mac keeps running. */

#define _POSIX_C_SOURCE 200809L

#include "geistshell/machine_state.h"

#include "geistshell/process_profile.h"

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
#    include <string.h>
#    include <unistd.h> /* sysconf: not pulled in transitively on glibc */

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

/* Under-voltage on a Pi 5 is an hwmon alarm, not a firmware sysfs node: the
 * /sys/devices/platform/soc/soc:firmware/get_throttled path this first used
 * does not exist on current kernels, which left the field permanently unknown
 * on the very hardware the roadmap targets. vcgencmd would also report events
 * since boot, but it stays a non-dependency by design, so "past" is only
 * reachable where the firmware node does exist. */
static enum spg_throttle_state read_throttle(const size_t cap, char buf[cap]) {
    size_t n = read_file("/sys/devices/platform/soc/soc:firmware/get_throttled",
                         cap, buf);
    if (n > 0u) {
        return spg_telemetry_parse_throttle(n, buf);
    }
    char path[96];
    for (unsigned i = 0u; i < 16u; i += 1u) {
        if (snprintf(path, sizeof path, "/sys/class/hwmon/hwmon%u/name", i) <
            0) {
            continue;
        }
        n = read_file(path, cap, buf);
        if (n < 8u || memcmp(buf, "rpi_volt", 8u) != 0) {
            continue;
        }
        if (snprintf(path, sizeof path,
                     "/sys/class/hwmon/hwmon%u/in0_lcrit_alarm", i) < 0) {
            continue;
        }
        n = read_file(path, cap, buf);
        if (n == 0u) {
            return SPG_THROTTLE_UNKNOWN;
        }
        return buf[0] == '0' ? SPG_THROTTLE_NONE : SPG_THROTTLE_ACTIVE;
    }
    return SPG_THROTTLE_UNKNOWN;
}

/* Read /proc/<pid>/stat for every numeric entry, then select the bounded set
 * that reaches the snapshot. Returns the total number seen, which is not the
 * number kept — the snapshot is deliberately smaller than the machine. */
static uint64_t enumerate_processes(
    const size_t prev_n, const struct spg_process_sample prev[],
    const uint64_t total_delta, const struct spg_process_profile *profile,
    struct spg_machine_state *out) {
    DIR *dir = opendir("/proc");
    if (dir == nullptr) {
        return SPG_MACHINE_UNKNOWN;
    }
    const long     page_size  = sysconf(_SC_PAGESIZE);
    const uint64_t page_bytes = page_size > 0 ? (uint64_t)page_size : 0u;

    struct spg_process_sample all[SPG_MACHINE_MAX_PROCESSES];
    size_t                    n_all = 0u;
    uint64_t                  total = 0u;
    char                      path[64];
    char                      buf[2048];

    const struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        bool numeric = entry->d_name[0] != '\0';
        for (size_t i = 0u; numeric && entry->d_name[i] != '\0'; i += 1u) {
            numeric = entry->d_name[i] >= '0' && entry->d_name[i] <= '9';
        }
        if (!numeric) {
            continue;
        }
        total += 1u;
        if (snprintf(path, sizeof path, "/proc/%s/stat", entry->d_name) < 0) {
            continue;
        }
        const size_t n = read_file(path, sizeof buf, buf);
        if (n == 0u) {
            continue; /* the process exited between readdir and open */
        }
        struct spg_process_sample sample = {};
        if (spg_process_parse_stat(n, buf, page_bytes, &sample) != SPG_OK) {
            continue;
        }
        /* Roles first: both the signal filter below and the ranking in
         * spg_process_select depend on knowing what is managed. */
        spg_process_apply_profile(profile, 1u, &sample);
        const struct spg_process_sample *before =
            spg_process_find(prev_n, prev, sample.pid, sample.start_identity);
        sample.cpu_bp =
            spg_process_utilisation_bp(before, &sample, total_delta);
        /* A process with no memory and no measurable CPU carries no signal.
         * On a Pi 5 that is ~100 kernel threads (kworker, rcu_ and migration threads),
         * which filled 58 of 64 context slots with ~4 KB of "(rss-bytes 0)"
         * during the hardware test. Managed processes are always offered: a
         * paused batch job that currently costs nothing is exactly what the
         * model must still see. */
        const bool has_signal =
            sample.rss_bytes != 0u && sample.rss_bytes != SPG_MACHINE_UNKNOWN;
        const bool has_cpu =
            sample.cpu_bp != SPG_MACHINE_UNKNOWN && sample.cpu_bp != 0u;
        if (!has_signal && !has_cpu &&
            sample.profile_index == SPG_PROCESS_NO_PROFILE) {
            continue;
        }
        (void)spg_process_offer(SPG_MACHINE_MAX_PROCESSES, all, &n_all,
                                &sample);
    }
    (void)closedir(dir);

    (void)spg_process_select(n_all, all, SPG_MACHINE_MAX_PROCESSES,
                             out->processes, &out->n_processes,
                             &out->processes_truncated);
    /* Every process was considered; the ones that did not make the cut are
     * still dropped, and the consumer must know the list is not the machine. */
    if (total > (uint64_t)out->n_processes) {
        out->processes_truncated = true;
    }
    return total;
}

enum spg_status spg_machine_sample_with_processes(
    const uint64_t timestamp_ns, const struct spg_cpu_sample *prev,
    const size_t prev_n, const struct spg_process_sample prev_procs[],
    const struct spg_process_profile *profile, struct spg_machine_state *out) {
    if (out == nullptr || (prev_n > 0u && prev_procs == nullptr)) {
        return SPG_E_INVALID_ARG;
    }
    state_init(out, timestamp_ns);

    /* One buffer, reused: /proc/meminfo is the largest of these by far. */
    char     buf[4096];
    uint64_t total_delta = 0u;
    size_t   n           = read_file("/proc/stat", sizeof buf, buf);
    if (n > 0u && spg_telemetry_parse_stat(n, buf, &out->cpu) == SPG_OK) {
        out->cpu_utilisation_bp = spg_telemetry_utilisation_bp(prev, &out->cpu);
        if (prev != nullptr && out->cpu.total >= prev->total) {
            total_delta = out->cpu.total - prev->total;
        }
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
    out->throttle = read_throttle(sizeof buf, buf);
    out->process_count =
        enumerate_processes(prev_n, prev_procs, total_delta, profile, out);
    return SPG_OK;
}

#else /* not Linux */

enum spg_status spg_machine_sample_with_processes(
    const uint64_t timestamp_ns, const struct spg_cpu_sample *prev,
    const size_t prev_n, const struct spg_process_sample prev_procs[],
    const struct spg_process_profile *profile, struct spg_machine_state *out) {
    (void)prev;
    (void)prev_procs;
    (void)profile;
    if (out == nullptr || (prev_n > 0u && prev_procs == nullptr)) {
        return SPG_E_INVALID_ARG;
    }
    state_init(out, timestamp_ns);
    return SPG_E_UNSUPPORTED;
}

#endif
/* Convenience for callers with no previous tick, e.g. the first sample and the
 * tests. */
enum spg_status spg_machine_sample(const uint64_t               timestamp_ns,
                                   const struct spg_cpu_sample *prev,
                                   struct spg_machine_state    *out) {
    return spg_machine_sample_with_processes(timestamp_ns, prev, 0u, nullptr,
                                             nullptr, out);
}
