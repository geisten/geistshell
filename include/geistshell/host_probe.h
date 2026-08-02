#ifndef GEISTSHELL_HOST_PROBE_H
#define GEISTSHELL_HOST_PROBE_H

#include "geistshell/status.h"

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

#ifdef __cplusplus
}
#endif

#endif
