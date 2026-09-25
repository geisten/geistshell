#ifndef GEISTSHELL_SANDBOX_H
#define GEISTSHELL_SANDBOX_H

#include "geistshell/status.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* OS-level isolation for governed shell commands.
 *
 * The containment that existed before this file -- own process group, wall
 * timeout, setrlimit -- bounds how LONG a command runs and how MUCH it
 * consumes, never WHAT it touches. Two holes followed from that (SECURITY.md,
 * "What the runtime does NOT do"): network denial was declarative, resting on
 * the model's self-declared uses_network flag, and `allowed_workdir_prefix`
 * constrained the launch directory, not what the command wrote. A command
 * declared network-free could open a socket; a command launched in a scratch
 * directory could write /etc via an absolute path.
 *
 * Both close by handing the command to the host's own isolation tool instead
 * of exec'ing it directly:
 *
 *   Linux   bwrap(1)         network namespace (loopback only), read-only
 *                            root, one writable bind mount plus /tmp
 *   macOS   sandbox-exec(1)  SBPL profile denying network* and every
 *                            file-write* outside the writable directory
 *
 * The spec is produced by the executor boundary from operator config, never
 * from model output -- which is precisely what the declarative check lacked.
 * A host with neither tool fails closed: the command does not start.
 *
 * ponytail: no seccomp-BPF program and no Landlock ruleset of our own. bwrap
 * gives the namespace guarantees this threat model needs in one execve; a
 * hand-written syscall filter is the upgrade path when a caller needs more
 * than "no network, one writable path". */

/* The wrapper prefix is fixed-shape (18 tokens at most, for bwrap with a
 * writable bind); the cap leaves room without inviting growth. */
#define SPG_SANDBOX_MAX_ARGV    24u
#define SPG_SANDBOX_PROFILE_CAP 1024u

struct spg_sandbox_spec {
    bool enabled;       /* false runs the command unwrapped (ungoverned paths) */
    bool allow_network; /* false removes every interface but loopback */
    /* The one writable path (besides the system temp directories, which both
     * mechanisms leave writable). Absolute, no quotes or
     * control bytes -- it is interpolated into an SBPL string. nullptr leaves
     * nothing but /tmp writable. */
    const char *rw_dir;
};

/* The argv prefix that turns a command into a sandboxed command: argv[0..argc)
 * followed by the caller's own argv. profile is the backing store for the
 * macOS profile argument, so one wrapper owns everything its argv points at
 * (nothing here allocates). */
struct spg_sandbox_wrapper {
    size_t      argc;
    const char *argv[SPG_SANDBOX_MAX_ARGV];
    char        profile[SPG_SANDBOX_PROFILE_CAP];
};

/* Absolute path of this host's sandbox binary, or nullptr when it has none. */
[[nodiscard]] const char *spg_sandbox_tool_path(void);

/* Build the wrapper for tool_path. Pure -- the mechanism follows tool_path's
 * basename rather than the host, so both shapes are testable on any platform.
 * Returns SPG_E_INVALID_ARG for a disabled spec or an rw_dir that is relative
 * or carries a quote, backslash or control byte, SPG_E_UNSUPPORTED for a tool
 * that is neither bwrap nor sandbox-exec, SPG_E_OVERFLOW if the prefix does not
 * fit (it is then left empty, never half-built). */
[[nodiscard]] enum spg_status
spg_sandbox_wrapper_build(const char *tool_path,
                          const struct spg_sandbox_spec *spec,
                          struct spg_sandbox_wrapper *out);

#ifdef __cplusplus
}
#endif

#endif
