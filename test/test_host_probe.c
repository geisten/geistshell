#include "geistshell/host_probe.h"

#include <stdio.h>
#include <string.h>

static int test_probe_basic(void) {
    struct spg_host_info info = {};
    if (spg_host_probe(&info) != SPG_OK) {
        return 1;
    }
    /* uname always yields a sysname; on supported hosts it classifies. */
    if (info.sysname[0] == '\0') {
        return 1;
    }
    /* Every field must be NUL-terminated within its buffer. */
    if (memchr(info.sysname, '\0', SPG_HOST_FIELD_CAP) == nullptr ||
        memchr(info.release, '\0', SPG_HOST_FIELD_CAP) == nullptr ||
        memchr(info.version, '\0', SPG_HOST_FIELD_CAP) == nullptr ||
        memchr(info.machine, '\0', SPG_HOST_FIELD_CAP) == nullptr ||
        memchr(info.nodename, '\0', SPG_HOST_FIELD_CAP) == nullptr) {
        return 1;
    }
    if (spg_host_os_to_string(info.os) == nullptr) {
        return 1;
    }
    return 0;
}

static int test_probe_null(void) {
    return spg_host_probe(nullptr) == SPG_E_INVALID_ARG ? 0 : 1;
}

static int test_os_strings(void) {
    const enum spg_host_os all[] = {
        SPG_HOST_OS_UNKNOWN, SPG_HOST_OS_LINUX,   SPG_HOST_OS_MACOS,
        SPG_HOST_OS_FREEBSD, SPG_HOST_OS_OPENBSD, SPG_HOST_OS_NETBSD,
    };
    for (size_t i = 0u; i < sizeof all / sizeof all[0]; i += 1u) {
        const char *s = spg_host_os_to_string(all[i]);
        if (s == nullptr || s[0] == '\0') {
            return 1;
        }
    }
    if (strcmp(spg_host_os_to_string(SPG_HOST_OS_UNKNOWN), "unknown") != 0 ||
        strcmp(spg_host_os_to_string(SPG_HOST_OS_LINUX), "linux") != 0) {
        return 1;
    }
    return 0;
}

/* The load average is the one field that must be internally consistent:
 * load1_per_cpu_bp is derived, so it has to agree with the two numbers it was
 * derived from. Everything else is "whatever the kernel said". */
static int test_telemetry_read(void) {
    struct spg_host_telemetry t = {};
    if (spg_host_telemetry_read(&t) != SPG_OK) {
        return 1;
    }
    if (t.has_cpu_count && t.cpu_count == 0u) {
        return 1;
    }
    if (t.has_load_per_cpu && (!t.has_load || !t.has_cpu_count)) {
        return 1;
    }
    if (t.has_load_per_cpu &&
        t.load1_per_cpu_bp !=
            (uint32_t)(((uint64_t)t.load1_centi * 100u) / t.cpu_count)) {
        return 1;
    }
    if (t.has_temperature && (t.temperature_mc < -50000 ||
                              t.temperature_mc > 200000)) {
        return 1;
    }
    return spg_host_telemetry_read(nullptr) == SPG_E_INVALID_ARG ? 0 : 1;
}

/* An absent field must be absent from the rendering, not present as a zero:
 * "(temp_mc 0)" on a machine with no thermal sensor is a lie the model would
 * have no way to catch. */
static int test_telemetry_render(void) {
    char        dst[SPG_HOST_STATUS_CAP] = {0};
    const struct spg_host_telemetry empty = {};
    if (spg_host_telemetry_render(&empty, sizeof dst, dst) != SPG_OK ||
        strcmp(dst, "(host_status)") != 0) {
        return 1;
    }

    const struct spg_host_telemetry full = {
        .has_cpu_count     = true,
        .cpu_count         = 4u,
        .has_load          = true,
        .load1_centi       = 137u,
        .load5_centi       = 98u,
        .load15_centi      = 76u,
        .has_load_per_cpu  = true,
        .load1_per_cpu_bp  = 3425u,
        .has_temperature   = true,
        .temperature_mc    = 52148,
        .has_process_count = true,
        .process_count     = 431u,
    };
    if (spg_host_telemetry_render(&full, sizeof dst, dst) != SPG_OK) {
        return 1;
    }
    if (strcmp(dst,
               "(host_status (cpus 4) (load1_centi 137) (load5_centi 98) "
               "(load15_centi 76) (load1_per_cpu_bp 3425) (temp_mc 52148) "
               "(processes 431))") != 0) {
        fprintf(stderr, "render mismatch: %s\n", dst);
        return 1;
    }

    /* A buffer that cannot hold the whole form fails and empties, so a caller
     * can never splice half an s-expression into the context. */
    char small[8] = {0};
    if (spg_host_telemetry_render(&full, sizeof small, small) != SPG_E_LIMIT ||
        small[0] != '\0') {
        return 1;
    }
    return spg_host_telemetry_render(nullptr, sizeof dst, dst) ==
                   SPG_E_INVALID_ARG
               ? 0
               : 1;
}

int main(void) {
    if (test_telemetry_read() != 0) {
        fprintf(stderr, "test_telemetry_read failed\n");
        return 1;
    }
    if (test_telemetry_render() != 0) {
        fprintf(stderr, "test_telemetry_render failed\n");
        return 1;
    }
    if (test_probe_basic() != 0) {
        fprintf(stderr, "test_probe_basic failed\n");
        return 1;
    }
    if (test_probe_null() != 0) {
        fprintf(stderr, "test_probe_null failed\n");
        return 1;
    }
    if (test_os_strings() != 0) {
        fprintf(stderr, "test_os_strings failed\n");
        return 1;
    }
    return 0;
}
