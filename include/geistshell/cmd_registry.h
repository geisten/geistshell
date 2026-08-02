#ifndef GEISTSHELL_CMD_REGISTRY_H
#define GEISTSHELL_CMD_REGISTRY_H

#include "geistshell/host_probe.h"
#include "geistshell/status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* OS-availability bits, one per spg_host_os value (bit index == enum value).
 * SPG_HOST_OS_UNKNOWN (0) is intentionally unused. */
enum spg_cmd_os_bit {
    SPG_CMD_OS_LINUX   = 1 << SPG_HOST_OS_LINUX,
    SPG_CMD_OS_MACOS   = 1 << SPG_HOST_OS_MACOS,
    SPG_CMD_OS_FREEBSD = 1 << SPG_HOST_OS_FREEBSD,
    SPG_CMD_OS_OPENBSD = 1 << SPG_HOST_OS_OPENBSD,
    SPG_CMD_OS_NETBSD  = 1 << SPG_HOST_OS_NETBSD,
    SPG_CMD_OS_BSD = SPG_CMD_OS_FREEBSD | SPG_CMD_OS_OPENBSD | SPG_CMD_OS_NETBSD,
    SPG_CMD_OS_ALL = SPG_CMD_OS_LINUX | SPG_CMD_OS_MACOS | SPG_CMD_OS_BSD,
};

/* A known command and the metadata callers need to validate and run it.
 *
 * Extensibility: adding a command is one entry in the table in cmd_registry.c.
 * The executor resolves the binary itself through PATH, so no absolute path is
 * stored here — this table is about *knowing* commands, not locating them. */
struct spg_cmd_descriptor {
    const char *name;         /* command name, e.g. "uname" */
    const char *common_flags; /* hint of common flags, e.g. "-a -s -r -m" */
    const char *summary;      /* one-line human description */
    uint32_t    os_mask;      /* bitwise-or of spg_cmd_os_bit values */
    uint64_t    default_timeout_ms;
    bool        uses_network; /* command reaches the network (e.g. ssh) */
};

/* Look up a command by exact name. Returns nullptr when unknown or name is
 * null. The returned pointer refers to static storage and outlives the call. */
[[nodiscard]] const struct spg_cmd_descriptor *
spg_cmd_registry_find(const char *name);

/* True when desc is non-null and marked available on the given OS. */
[[nodiscard]] bool
spg_cmd_registry_available(const struct spg_cmd_descriptor *desc,
                           enum spg_host_os                 os);

/* Number of entries in the registry, and indexed access for listing/testing.
 * spg_cmd_registry_at returns nullptr when index is out of range. */
[[nodiscard]] size_t spg_cmd_registry_count(void);
[[nodiscard]] const struct spg_cmd_descriptor *
spg_cmd_registry_at(size_t index);

#ifdef __cplusplus
}
#endif

#endif
