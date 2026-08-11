#ifndef GEISTSHELL_CMD_MENU_H
#define GEISTSHELL_CMD_MENU_H

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

/* THE COMMAND MENU IS NOT AN ALLOWLIST. Read this before using it as one.
 *
 * It is a PROPOSAL SPACE FOR THE MODEL: a list of commands the agent is told
 * about, with a one-line description and a flag hint, rendered into the model's
 * context and used to mask the first token of a decoded `command` value. Its
 * purpose is DISCOVERABILITY — a model that was never tool-trained cannot
 * invent a sensible shell command from nothing.
 *
 * The security boundary is somewhere else entirely and is unaffected by this
 * file: spg_executor_boundary_check (capability, workdir prefix, network flag,
 * timeout, output cap, cleared env) plus the OS sandbox in cmd_executor
 * (fork+exec, setrlimit, process-group kill). A command absent from this menu
 * still runs if it gets past those; a command present in it still does not run
 * if it does not.
 *
 * That is deliberate, not an oversight. A name filter on argv[0] is worthless
 * against `sh -c`, and making it real would mean banning shell metacharacters,
 * interpreters and path invocations — which is `local_shell` abolished, and
 * with it the reason geistshell is not geistagent. A half allowlist is worse
 * than none, because it gets believed.
 *
 * Corollary, and the reason this paragraph exists: once the menu is loaded
 * from a file, that file is a MODEL INPUT in the same class as the scenario
 * and corpus text — untrusted. It may influence what the model PROPOSES and
 * never what the executor PERMITS. If it ever did both, editing a data row
 * would be a privilege escalation.
 *
 * The executor's only use of an entry is advisory metadata (default timeout,
 * network flag). It resolves the binary through PATH itself, so no absolute
 * path is stored here. */
struct spg_cmd_menu_entry {
    const char *name;         /* command name, e.g. "uname" */
    const char *common_flags; /* hint of common flags, e.g. "-a -s -r -m" */
    const char *summary;      /* one-line human description */
    uint32_t    os_mask;      /* bitwise-or of spg_cmd_os_bit values */
    uint64_t    default_timeout_ms;
    bool        uses_network; /* command reaches the network (e.g. ssh) */
};

/* Look up a menu entry by exact name. Returns nullptr when the command is not
 * on the menu or name is null — which tells you nothing about whether it may
 * run. The returned pointer refers to static storage and outlives the call. */
[[nodiscard]] const struct spg_cmd_menu_entry *
spg_cmd_menu_find(const char *name);

/* True when desc is non-null and marked available on the given OS. */
[[nodiscard]] bool
spg_cmd_menu_available(const struct spg_cmd_menu_entry *desc,
                           enum spg_host_os                 os);

/* Number of entries in the registry, and indexed access for listing/testing.
 * spg_cmd_menu_at returns nullptr when index is out of range. */
[[nodiscard]] size_t spg_cmd_menu_count(void);
[[nodiscard]] const struct spg_cmd_menu_entry *
spg_cmd_menu_at(size_t index);

/* Render the menu as the context's (tools ...) section: one line per command
 * available on `os`, name + summary + flag hint. Writes a NUL-terminated
 * string and returns its length (0 when nothing fits or nothing is available).
 *
 * This is the half that was missing for most of this file's life: the table
 * carried `summary` and `common_flags`, which only a model can use, and no
 * model ever saw them. */
size_t spg_cmd_menu_render(enum spg_host_os os, size_t cap, char out[]);

/* A menu loaded from a file, replacing the built-in table for one run.
 *
 * UNTRUSTED INPUT. This file reaches the model's context and the decoder mask;
 * it reaches the executor's permission decision nowhere. Treat it like the
 * scenario and corpus text, and never let a future change give it authority —
 * a data row must not become a privilege. */
#define SPG_CMD_MENU_MAX 64u

struct spg_cmd_menu {
    size_t                    count;
    struct spg_cmd_menu_entry entries[SPG_CMD_MENU_MAX];
    /* Backing store for the borrowed name/summary/flags strings. */
    char                      text[8192];
    size_t                    text_used;
};

/* Parse a (command_menu ((name "ls") (summary "list files") (flags "-l")) ...)
 * form. `flags` is optional. Entries are available on every OS: a file says
 * what the operator wants offered, and second-guessing that per platform would
 * make the same file mean different things on different hosts.
 *
 * Returns SPG_E_FORMAT on a parse error, SPG_E_SCHEMA on a bad shape,
 * SPG_E_LIMIT when the file exceeds SPG_CMD_MENU_MAX entries or the text
 * store. A malformed menu is an error, never a silently shorter menu. */
[[nodiscard]] enum spg_status spg_cmd_menu_load(size_t input_n,
                                                const char input[static 1],
                                                struct spg_cmd_menu *out);

/* Render a loaded menu, same shape as spg_cmd_menu_render. */
size_t spg_cmd_menu_render_of(const struct spg_cmd_menu *menu, size_t cap,
                              char out[]);

/* Fill names[0..cap) with the menu's command names for the decoder mask and
 * return the count. Borrowed from the menu, which must outlive them. */
size_t spg_cmd_menu_names(const struct spg_cmd_menu *menu, size_t cap,
                          const char *names[]);

/* The built-in table's names, for the same mask when no menu file is given. */
size_t spg_cmd_menu_builtin_names(enum spg_host_os os, size_t cap,
                                  const char *names[]);

#ifdef __cplusplus
}
#endif

#endif
