#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#    define _DARWIN_C_SOURCE 1
#endif
/* getloadavg(3) is BSD, not POSIX: glibc hides it behind _DEFAULT_SOURCE. */
#define _DEFAULT_SOURCE 1

#include "geistshell/host_probe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

#if defined(__linux__)
#    include <ctype.h>
#    include <dirent.h>
#elif defined(__APPLE__)
#    include <sys/sysctl.h>
#endif

/* Copy a NUL-terminated source into a fixed destination, truncating to fit and
 * always terminating. cap is guaranteed > 0 by the struct definition. */
static void copy_field(char *dst, const size_t cap, const char *src) {
    size_t i = 0u;
    if (src != nullptr) {
        for (; i + 1u < cap && src[i] != '\0'; i += 1u) {
            dst[i] = src[i];
        }
    }
    dst[i] = '\0';
}

static enum spg_host_os classify(const char *sysname) {
    if (sysname == nullptr) {
        return SPG_HOST_OS_UNKNOWN;
    }
    if (strcmp(sysname, "Linux") == 0) {
        return SPG_HOST_OS_LINUX;
    }
    if (strcmp(sysname, "Darwin") == 0) {
        return SPG_HOST_OS_MACOS;
    }
    if (strcmp(sysname, "FreeBSD") == 0) {
        return SPG_HOST_OS_FREEBSD;
    }
    if (strcmp(sysname, "OpenBSD") == 0) {
        return SPG_HOST_OS_OPENBSD;
    }
    if (strcmp(sysname, "NetBSD") == 0) {
        return SPG_HOST_OS_NETBSD;
    }
    return SPG_HOST_OS_UNKNOWN;
}

enum spg_status spg_host_probe(struct spg_host_info *out) {
    if (out == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out = (struct spg_host_info){};

    struct utsname uts;
    if (uname(&uts) != 0) {
        return SPG_E_IO;
    }
    copy_field(out->sysname, sizeof out->sysname, uts.sysname);
    copy_field(out->release, sizeof out->release, uts.release);
    copy_field(out->version, sizeof out->version, uts.version);
    copy_field(out->machine, sizeof out->machine, uts.machine);
    copy_field(out->nodename, sizeof out->nodename, uts.nodename);
    out->os = classify(out->sysname);
    return SPG_OK;
}

/* --- live telemetry ------------------------------------------------------ */

static void read_cpu_count(struct spg_host_telemetry *out) {
    const long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n > 0) {
        out->cpu_count     = (uint32_t)n;
        out->has_cpu_count = true;
    }
}

/* getloadavg reports doubles; we keep two decimals as an integer so nothing
 * downstream has to deal with a float. Negative or absurd values are dropped
 * rather than clamped — a bogus load is worse than no load. */
static void read_load(struct spg_host_telemetry *out) {
    double avg[3] = {0.0, 0.0, 0.0};
    if (getloadavg(avg, 3) != 3) {
        return;
    }
    for (size_t i = 0u; i < 3u; i += 1u) {
        if (avg[i] < 0.0 || avg[i] > 100000.0) {
            return;
        }
    }
    out->load1_centi  = (uint32_t)(avg[0] * 100.0);
    out->load5_centi  = (uint32_t)(avg[1] * 100.0);
    out->load15_centi = (uint32_t)(avg[2] * 100.0);
    out->has_load     = true;

    /* Derived from load1_centi, not from the double, so the two numbers in the
     * context can never disagree about the same machine. */
    if (out->has_cpu_count && out->cpu_count > 0u) {
        out->load1_per_cpu_bp =
            (uint32_t)(((uint64_t)out->load1_centi * 100u) / out->cpu_count);
        out->has_load_per_cpu = true;
    }
}

static void read_temperature(struct spg_host_telemetry *out) {
#if defined(__linux__)
    /* ponytail: thermal_zone0 only. On a Pi 5 that zone is the SoC, which is
     * the one that throttles; a board with several zones needs a scan and a
     * type= check. Add that when a second zone actually matters. */
    FILE *f = fopen("/sys/class/thermal/thermal_zone0/temp", "rb");
    if (f == nullptr) {
        return;
    }
    char   line[32];
    char  *got = fgets(line, (int)sizeof line, f);
    (void)fclose(f);
    if (got == nullptr) {
        return;
    }
    char      *end   = nullptr;
    const long milli = strtol(line, &end, 10);
    if (end == line || milli < -50000 || milli > 200000) {
        return;
    }
    out->temperature_mc  = (int32_t)milli;
    out->has_temperature = true;
#else
    /* ponytail: macOS has no unprivileged CPU-temperature interface — it needs
     * IOKit/SMC or root powermetrics, neither of which belongs in a sandboxed
     * agent runtime. Reported as absent, which is the honest answer. */
    (void)out;
#endif
}

static void read_process_count(struct spg_host_telemetry *out) {
#if defined(__linux__)
    DIR *d = opendir("/proc");
    if (d == nullptr) {
        return;
    }
    uint32_t       n = 0u;
    struct dirent *e = nullptr;
    while ((e = readdir(d)) != nullptr) {
        if (isdigit((unsigned char)e->d_name[0])) {
            n += 1u;
        }
    }
    (void)closedir(d);
    out->process_count     = n;
    out->has_process_count = true;
#elif defined(__APPLE__)
    /* A null buffer turns this into a size query, so no allocation is needed to
     * count what would have been returned. */
    int    mib[]  = {CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0};
    size_t needed = 0u;
    if (sysctl(mib, 4u, nullptr, &needed, nullptr, 0u) != 0 || needed == 0u) {
        return;
    }
    out->process_count     = (uint32_t)(needed / sizeof(struct kinfo_proc));
    out->has_process_count = true;
#else
    (void)out;
#endif
}

enum spg_status spg_host_telemetry_read(struct spg_host_telemetry *out) {
    if (out == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out = (struct spg_host_telemetry){};
    read_cpu_count(out);
    read_load(out); /* after the CPU count: load-per-cpu needs it */
    read_temperature(out);
    read_process_count(out);
    return SPG_OK;
}

enum spg_status
spg_host_telemetry_render(const struct spg_host_telemetry *telemetry,
                          const size_t dst_capacity,
                          char dst[static const dst_capacity]) {
    if (telemetry == nullptr || dst == nullptr || dst_capacity == 0u) {
        return SPG_E_INVALID_ARG;
    }
    /* Built incrementally rather than as one big snprintf: the fields are
     * conditional, and a single format string would have to be kept in sync
     * with the flags by hand. Any overflow empties the buffer and fails. */
    size_t used = 0u;

#define APPEND(...)                                                            \
    do {                                                                       \
        const int written =                                                    \
            snprintf(dst + used, dst_capacity - used, __VA_ARGS__);            \
        if (written < 0 || (size_t)written >= dst_capacity - used) {           \
            dst[0] = '\0';                                                     \
            return SPG_E_LIMIT;                                                \
        }                                                                      \
        used += (size_t)written;                                               \
    } while (0)

    APPEND("(host_status");
    if (telemetry->has_cpu_count) {
        APPEND(" (cpus %u)", telemetry->cpu_count);
    }
    if (telemetry->has_load) {
        APPEND(" (load1_centi %u) (load5_centi %u) (load15_centi %u)",
               telemetry->load1_centi, telemetry->load5_centi,
               telemetry->load15_centi);
    }
    if (telemetry->has_load_per_cpu) {
        APPEND(" (load1_per_cpu_bp %u)", telemetry->load1_per_cpu_bp);
    }
    if (telemetry->has_temperature) {
        APPEND(" (temp_mc %d)", telemetry->temperature_mc);
    }
    if (telemetry->has_process_count) {
        APPEND(" (processes %u)", telemetry->process_count);
    }
    APPEND(")");
#undef APPEND

    return SPG_OK;
}

const char *spg_host_os_to_string(const enum spg_host_os os) {
    switch (os) {
    case SPG_HOST_OS_UNKNOWN:
        return "unknown";
    case SPG_HOST_OS_LINUX:
        return "linux";
    case SPG_HOST_OS_MACOS:
        return "macos";
    case SPG_HOST_OS_FREEBSD:
        return "freebsd";
    case SPG_HOST_OS_OPENBSD:
        return "openbsd";
    case SPG_HOST_OS_NETBSD:
        return "netbsd";
    }
    return "unknown";
}
