/* Linux backend: /proc and /sys.
 *
 * Extracted from the sampler so the sampler no longer knows what an operating
 * system is. The parsers stay where they were — they are pure functions over
 * buffers and belong to the portable half; only the reading of files lives
 * here. */

#define _POSIX_C_SOURCE 200809L

#include "geistshell/machine_backend.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h> /* sysconf: not pulled in transitively on glibc */

const char *spg_backend_name(void) { return "linux"; }
bool        spg_backend_is_live(void) { return true; }

/* Caller-provided buffer, no allocation. Returns the byte count read, or 0
 * when the file is missing — a missing sysfs file is normal, not an error. */
static size_t read_file(const char *path, const size_t cap, char buf[cap]) {
    FILE *f = fopen(path, "rbe");
    if (f == nullptr) {
        return 0u;
    }
    const size_t n = fread(buf, 1u, cap, f);
    (void)fclose(f);
    return n;
}

enum spg_status spg_backend_cpu(struct spg_cpu_sample *out) {
    if (out == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out = (struct spg_cpu_sample){};
    char         buf[4096];
    const size_t n = read_file("/proc/stat", sizeof buf, buf);
    if (n == 0u) {
        return SPG_E_UNSUPPORTED;
    }
    return spg_telemetry_parse_stat(n, buf, out);
}

enum spg_status spg_backend_memory(struct spg_memory_sample *out) {
    if (out == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    char         buf[4096];
    const size_t n = read_file("/proc/meminfo", sizeof buf, buf);
    if (n == 0u) {
        *out =
            (struct spg_memory_sample){.total_bytes     = SPG_MACHINE_UNKNOWN,
                                       .used_bytes      = SPG_MACHINE_UNKNOWN,
                                       .swap_used_bytes = SPG_MACHINE_UNKNOWN};
        return SPG_E_UNSUPPORTED;
    }
    return spg_telemetry_parse_meminfo(n, buf, out);
}

enum spg_status spg_backend_load(struct spg_load_sample *out) {
    if (out == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    char         buf[256];
    const size_t n = read_file("/proc/loadavg", sizeof buf, buf);
    if (n == 0u) {
        *out = (struct spg_load_sample){.avg_1_cbp  = SPG_MACHINE_UNKNOWN,
                                        .avg_5_cbp  = SPG_MACHINE_UNKNOWN,
                                        .avg_15_cbp = SPG_MACHINE_UNKNOWN};
        return SPG_E_UNSUPPORTED;
    }
    return spg_telemetry_parse_loadavg(n, buf, out);
}

enum spg_status spg_backend_temperature(int64_t *out_mc) {
    if (out_mc == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out_mc = SPG_MACHINE_UNKNOWN_S;
    char         buf[64];
    const size_t n =
        read_file("/sys/class/thermal/thermal_zone0/temp", sizeof buf, buf);
    if (n == 0u) {
        return SPG_E_UNSUPPORTED;
    }
    return spg_telemetry_parse_int(n, buf, out_mc);
}

enum spg_status spg_backend_frequency_khz(uint64_t *out_khz) {
    if (out_khz == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out_khz = SPG_MACHINE_UNKNOWN;
    char         buf[64];
    const size_t n =
        read_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq",
                  sizeof buf, buf);
    if (n == 0u) {
        return SPG_E_UNSUPPORTED;
    }
    int64_t khz = 0;
    if (spg_telemetry_parse_int(n, buf, &khz) != SPG_OK || khz < 0) {
        return SPG_E_FORMAT;
    }
    *out_khz = (uint64_t)khz;
    return SPG_OK;
}

/* Under-voltage on a Pi 5 is an hwmon alarm, not a firmware sysfs node: the
 * firmware path does not exist on current kernels, which left the field
 * permanently unknown on the very hardware the roadmap targets. */
enum spg_status spg_backend_throttle(enum spg_throttle_state *out) {
    if (out == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out = SPG_THROTTLE_UNKNOWN;
    char   buf[64];
    size_t n = read_file("/sys/devices/platform/soc/soc:firmware/get_throttled",
                         sizeof buf, buf);
    if (n > 0u) {
        *out = spg_telemetry_parse_throttle(n, buf);
        return SPG_OK;
    }
    char path[96];
    for (unsigned i = 0u; i < 16u; i += 1u) {
        if (snprintf(path, sizeof path, "/sys/class/hwmon/hwmon%u/name", i) <
            0) {
            continue;
        }
        n = read_file(path, sizeof buf, buf);
        if (n < 8u || memcmp(buf, "rpi_volt", 8u) != 0) {
            continue;
        }
        if (snprintf(path, sizeof path,
                     "/sys/class/hwmon/hwmon%u/in0_lcrit_alarm", i) < 0) {
            continue;
        }
        n = read_file(path, sizeof buf, buf);
        if (n == 0u) {
            return SPG_E_UNSUPPORTED;
        }
        *out = buf[0] == '0' ? SPG_THROTTLE_NONE : SPG_THROTTLE_ACTIVE;
        return SPG_OK;
    }
    return SPG_E_UNSUPPORTED;
}

enum spg_status spg_backend_processes(const size_t              cap,
                                      struct spg_process_sample out[],
                                      size_t *out_n, uint64_t *out_total) {
    if (out_n == nullptr || out_total == nullptr ||
        (cap > 0u && out == nullptr)) {
        return SPG_E_INVALID_ARG;
    }
    *out_n     = 0u;
    *out_total = 0u;

    DIR *dir = opendir("/proc");
    if (dir == nullptr) {
        return SPG_E_UNSUPPORTED;
    }
    const long     page_size  = sysconf(_SC_PAGESIZE);
    const uint64_t page_bytes = page_size > 0 ? (uint64_t)page_size : 0u;
    char           path[64];
    char           buf[2048];

    const struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        bool numeric = entry->d_name[0] != '\0';
        for (size_t i = 0u; numeric && entry->d_name[i] != '\0'; i += 1u) {
            numeric = entry->d_name[i] >= '0' && entry->d_name[i] <= '9';
        }
        if (!numeric) {
            continue;
        }
        *out_total += 1u;
        if (*out_n >= cap) {
            continue; /* still counted: the caller reports what it dropped */
        }
        if (snprintf(path, sizeof path, "/proc/%s/stat", entry->d_name) < 0) {
            continue;
        }
        const size_t n = read_file(path, sizeof buf, buf);
        if (n == 0u) {
            continue; /* it exited between readdir and open */
        }
        if (spg_process_parse_stat(n, buf, page_bytes, &out[*out_n]) ==
            SPG_OK) {
            *out_n += 1u;
        }
    }
    (void)closedir(dir);
    return SPG_OK;
}

enum spg_status spg_backend_process_identity(const uint64_t pid,
                                             uint64_t *out_start_identity) {
    if (out_start_identity == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out_start_identity = 0u;
    char path[64];
    if (snprintf(path, sizeof path, "/proc/%llu/stat",
                 (unsigned long long)pid) < 0) {
        return SPG_E_NOT_FOUND;
    }
    char         buf[2048];
    const size_t n = read_file(path, sizeof buf, buf);
    if (n == 0u) {
        return SPG_E_NOT_FOUND;
    }
    struct spg_process_sample sample = {};
    if (spg_process_parse_stat(n, buf, 4096u, &sample) != SPG_OK) {
        return SPG_E_NOT_FOUND;
    }
    *out_start_identity = sample.start_identity;
    return SPG_OK;
}

/* The pwmfan hwmon device, found by name rather than a fixed hwmonN path: the
 * numbering depends on probe order and changes across boots. */
static bool fan_dir(char out[static 64]) {
    for (unsigned i = 0u; i < 16u; i += 1u) {
        char path[96];
        if (snprintf(path, sizeof path, "/sys/class/hwmon/hwmon%u/name", i) <
            0) {
            continue;
        }
        char         name[32] = {};
        const size_t n        = read_file(path, sizeof name - 1u, name);
        if (n >= 6u && memcmp(name, "pwmfan", 6u) == 0) {
            return snprintf(out, 64u, "/sys/class/hwmon/hwmon%u", i) > 0;
        }
    }
    return false;
}

static bool read_u64_file(const char *path, uint64_t *out) {
    char         buf[32] = {};
    const size_t n       = read_file(path, sizeof buf - 1u, buf);
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

enum spg_status spg_backend_fan_read(uint64_t *out_rpm, uint64_t *out_duty) {
    if (out_rpm == nullptr || out_duty == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out_rpm  = SPG_MACHINE_UNKNOWN;
    *out_duty = SPG_MACHINE_UNKNOWN;
    char dir[64];
    if (!fan_dir(dir)) {
        return SPG_E_UNSUPPORTED;
    }
    char path[128];
    if (snprintf(path, sizeof path, "%s/fan1_input", dir) > 0) {
        (void)read_u64_file(path, out_rpm);
    }
    if (snprintf(path, sizeof path, "%s/pwm1", dir) > 0) {
        (void)read_u64_file(path, out_duty);
    }
    return SPG_OK;
}

enum spg_status spg_backend_fan_write(const uint64_t duty) {
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
