#ifndef GEISTSHELL_HOST_PROBE_H
#define GEISTSHELL_HOST_PROBE_H

#include "geistshell/status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Coarse classification of the local operating system, derived from the
 * uname(2) sysname. Extend by adding a value here and a case in the classifier
 * and the to_string helper. */
enum spg_host_os {
    SPG_HOST_OS_UNKNOWN = 0,
    SPG_HOST_OS_LINUX,
    SPG_HOST_OS_MACOS,
    SPG_HOST_OS_FREEBSD,
    SPG_HOST_OS_OPENBSD,
    SPG_HOST_OS_NETBSD,
};

/* Per-field buffer size. uname fields are short in practice (<= 256 on the
 * supported platforms); longer values are truncated with NUL termination. */
#define SPG_HOST_FIELD_CAP 256u

/* Snapshot of the local host. All char fields are always NUL-terminated. */
struct spg_host_info {
    enum spg_host_os os;
    char             sysname[SPG_HOST_FIELD_CAP];  /* e.g. "Linux", "Darwin" */
    char             release[SPG_HOST_FIELD_CAP];  /* kernel release */
    char             version[SPG_HOST_FIELD_CAP];  /* kernel version string */
    char             machine[SPG_HOST_FIELD_CAP];  /* arch, e.g. "arm64" */
    char             nodename[SPG_HOST_FIELD_CAP]; /* host name */
};

/* Fill out from uname(2). Returns SPG_E_INVALID_ARG on a null pointer and
 * SPG_E_IO if uname fails. On any failure out is left zero-initialized. */
[[nodiscard]] enum spg_status spg_host_probe(struct spg_host_info *out);

[[nodiscard]] const char *spg_host_os_to_string(enum spg_host_os os);

/* Live host telemetry: unlike spg_host_info this changes between ticks, so it
 * is read fresh each time it is rendered. Integers only, in the same spirit as
 * the _bp convention elsewhere: no float ever reaches the context.
 *
 * Every field has a `has_` companion because each one comes from a different
 * OS interface and any of them can be absent (a container with no loadavg, a
 * board with no thermal zone, a kernel that will not enumerate processes to an
 * unprivileged caller). Absent is rendered as absent, never as zero. */
struct spg_host_telemetry {
    bool     has_cpu_count;
    uint32_t cpu_count; /* online CPUs */

    bool     has_load;
    uint32_t load1_centi; /* 1-minute load average x100 */
    uint32_t load5_centi;
    uint32_t load15_centi;
    /* load1 / cpu_count in basis points: 10000 = one runnable thread per core.
     * This is the number to compare against a threshold; the raw load average
     * is meaningless without the core count beside it. */
    bool     has_load_per_cpu;
    uint32_t load1_per_cpu_bp;

    bool    has_temperature;
    int32_t temperature_mc; /* milli-degrees Celsius */

    bool     has_process_count;
    uint32_t process_count;
};

/* Reads whatever the platform will give up. Returns SPG_E_INVALID_ARG on a null
 * pointer, otherwise SPG_OK with the has_* flags telling you what was actually
 * available — a host that answers nothing is not an error, it is a host that
 * answers nothing. */
[[nodiscard]] enum spg_status
spg_host_telemetry_read(struct spg_host_telemetry *out);

/* Renders one `(host_status ...)` line, omitting unavailable fields, into a
 * caller-provided buffer. SPG_HOST_STATUS_CAP is enough for every field; a
 * smaller buffer that does not fit returns SPG_E_LIMIT and leaves dst empty. */
#define SPG_HOST_STATUS_CAP 192u

[[nodiscard]] enum spg_status
spg_host_telemetry_render(const struct spg_host_telemetry *telemetry,
                          size_t dst_capacity, char dst[static dst_capacity]);

#ifdef __cplusplus
}
#endif

#endif
