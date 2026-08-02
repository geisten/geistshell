#include "geistshell/cmd_registry.h"

#include <string.h>

/* The known-command table. Add a command by appending one entry. Keep names
 * unique; lookup is linear (the table is small) and order does not matter. */
static const struct spg_cmd_descriptor k_registry[] = {
    {.name               = "uname",
     .common_flags       = "-a -s -r -m -p",
     .summary            = "print system / kernel information",
     .os_mask            = SPG_CMD_OS_ALL,
     .default_timeout_ms = 2000u,
     .uses_network       = false},
    {.name               = "hostname",
     .common_flags       = "-s -f",
     .summary            = "print the host name",
     .os_mask            = SPG_CMD_OS_ALL,
     .default_timeout_ms = 2000u,
     .uses_network       = false},
    {.name               = "id",
     .common_flags       = "-u -g -n -G",
     .summary            = "print user and group identity",
     .os_mask            = SPG_CMD_OS_ALL,
     .default_timeout_ms = 2000u,
     .uses_network       = false},
    {.name               = "whoami",
     .common_flags       = "",
     .summary            = "print the effective user name",
     .os_mask            = SPG_CMD_OS_ALL,
     .default_timeout_ms = 2000u,
     .uses_network       = false},
    {.name               = "echo",
     .common_flags       = "-n",
     .summary            = "write arguments to standard output",
     .os_mask            = SPG_CMD_OS_ALL,
     .default_timeout_ms = 2000u,
     .uses_network       = false},
    {.name               = "ls",
     .common_flags       = "-l -a -h -R",
     .summary            = "list directory contents",
     .os_mask            = SPG_CMD_OS_ALL,
     .default_timeout_ms = 5000u,
     .uses_network       = false},
    {.name               = "cat",
     .common_flags       = "-n",
     .summary            = "concatenate and print files",
     .os_mask            = SPG_CMD_OS_ALL,
     .default_timeout_ms = 5000u,
     .uses_network       = false},
    /* ps flags differ across kernels: BSD-style "aux" is widely accepted, but
     * Linux procps also supports "-ef". The flag hint reflects both. */
    {.name               = "ps",
     .common_flags       = "aux -ef",
     .summary            = "report process status",
     .os_mask            = SPG_CMD_OS_ALL,
     .default_timeout_ms = 5000u,
     .uses_network       = false},
    {.name               = "df",
     .common_flags       = "-h -k",
     .summary            = "report file system disk usage",
     .os_mask            = SPG_CMD_OS_ALL,
     .default_timeout_ms = 5000u,
     .uses_network       = false},
    {.name               = "ssh",
     .common_flags       = "-p -i -o -l",
     .summary            = "OpenSSH remote login client",
     .os_mask            = SPG_CMD_OS_ALL,
     .default_timeout_ms = 10000u,
     .uses_network       = true},
};

static const size_t k_registry_count =
    sizeof k_registry / sizeof k_registry[0];

const struct spg_cmd_descriptor *spg_cmd_registry_find(const char *name) {
    if (name == nullptr) {
        return nullptr;
    }
    for (size_t i = 0u; i < k_registry_count; i += 1u) {
        if (strcmp(k_registry[i].name, name) == 0) {
            return &k_registry[i];
        }
    }
    return nullptr;
}

bool spg_cmd_registry_available(const struct spg_cmd_descriptor *desc,
                                const enum spg_host_os           os) {
    if (desc == nullptr) {
        return false;
    }
    const uint32_t bit = (uint32_t)1u << (unsigned)os;
    return (desc->os_mask & bit) != 0u;
}

size_t spg_cmd_registry_count(void) {
    return k_registry_count;
}

const struct spg_cmd_descriptor *spg_cmd_registry_at(const size_t index) {
    if (index >= k_registry_count) {
        return nullptr;
    }
    return &k_registry[index];
}
