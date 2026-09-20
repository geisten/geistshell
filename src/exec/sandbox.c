#define _POSIX_C_SOURCE 200809L

#include "geistshell/sandbox.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Absolute paths only: resolving the tool through PATH would let whoever
 * controls the environment swap the sandbox for a passthrough. */
static const char *const TOOL_CANDIDATES[] = {
#if defined(__APPLE__)
    "/usr/bin/sandbox-exec",
#else
    "/usr/bin/bwrap",
    "/bin/bwrap",
    "/usr/local/bin/bwrap",
#endif
};

const char *spg_sandbox_tool_path(void) {
    for (size_t i = 0u; i < sizeof TOOL_CANDIDATES / sizeof TOOL_CANDIDATES[0];
         i += 1u) {
        if (access(TOOL_CANDIDATES[i], X_OK) == 0) {
            return TOOL_CANDIDATES[i];
        }
    }
    return nullptr;
}

/* Count past the cap instead of clamping: a silently dropped argument is a
 * silently weakened sandbox, so the caller must be able to see the overflow. */
static void push(struct spg_sandbox_wrapper *w, const char *arg) {
    if (w->argc < SPG_SANDBOX_MAX_ARGV) {
        w->argv[w->argc] = arg;
    }
    w->argc += 1u;
}

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash != nullptr ? slash + 1u : path;
}

static bool rw_dir_valid(const char *dir) {
    if (dir == nullptr) {
        return true; /* nothing writable is a valid (stricter) sandbox */
    }
    if (dir[0] != '/') {
        return false; /* relative would bind/allow the wrong directory */
    }
    for (const char *p = dir; *p != '\0'; p += 1u) {
        if (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20u) {
            return false;
        }
    }
    return true;
}

/* bwrap: unshare everything, then rebuild a usable view. The root is bound
 * read-only rather than hidden -- commands need /usr, /etc and the interpreter
 * to run at all, and READ containment is not what is claimed here. /proc must
 * be remounted because the PID namespace is new, and /tmp is private so a
 * command cannot drop payloads for the next one. */
static void build_bwrap(const struct spg_sandbox_spec *spec, const char *tool,
                        struct spg_sandbox_wrapper *out) {
    push(out, tool);
    push(out, "--die-with-parent"); /* no orphan outliving the timeout kill */
    push(out, "--new-session");     /* own session: no TIOCSTI into our tty */
    push(out, "--unshare-all");
    if (spec->allow_network) {
        push(out, "--share-net");
    }
    push(out, "--ro-bind");
    push(out, "/");
    push(out, "/");
    push(out, "--dev");
    push(out, "/dev");
    push(out, "--proc");
    push(out, "/proc");
    push(out, "--tmpfs");
    push(out, "/tmp");
    if (spec->rw_dir != nullptr) {
        push(out, "--bind");
        push(out, spec->rw_dir);
        push(out, spec->rw_dir);
    }
    push(out, "--");
}

/* macOS: `allow default` minus the two things we actually deny, because an
 * allowlist profile cannot be written without knowing the command. TMPDIR
 * lives under /private/var/folders and tools that cannot write a temp file
 * fail in ways unrelated to the boundary being tested. */
static enum spg_status build_seatbelt(const struct spg_sandbox_spec *spec,
                                      const char                    *tool,
                                      struct spg_sandbox_wrapper    *out) {
    const int n = snprintf(
        out->profile, sizeof out->profile,
        "(version 1)(allow default)%s(deny file-write*)"
        "(allow file-write* (subpath \"/private/tmp\")"
        " (subpath \"/private/var/tmp\") (subpath \"/private/var/folders\")%s%s%s)",
        spec->allow_network ? "" : "(deny network*)",
        spec->rw_dir != nullptr ? " (subpath \"" : "",
        spec->rw_dir != nullptr ? spec->rw_dir : "",
        spec->rw_dir != nullptr ? "\")" : "");
    if (n < 0 || (size_t)n >= sizeof out->profile) {
        return SPG_E_OVERFLOW;
    }
    push(out, tool);
    push(out, "-p");
    push(out, out->profile);
    push(out, "--");
    return SPG_OK;
}

enum spg_status spg_sandbox_wrapper_build(const char                    *tool_path,
                                          const struct spg_sandbox_spec *spec,
                                          struct spg_sandbox_wrapper    *out) {
    if (tool_path == nullptr || tool_path[0] == '\0' || spec == nullptr ||
        out == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out = (struct spg_sandbox_wrapper){};
    if (!spec->enabled || !rw_dir_valid(spec->rw_dir)) {
        return SPG_E_INVALID_ARG;
    }

    const char     *tool   = base_name(tool_path);
    enum spg_status status = SPG_OK;
    if (strcmp(tool, "bwrap") == 0) {
        build_bwrap(spec, tool_path, out);
    } else if (strcmp(tool, "sandbox-exec") == 0) {
        status = build_seatbelt(spec, tool_path, out);
    } else {
        status = SPG_E_UNSUPPORTED;
    }

    if (status == SPG_OK && out->argc > SPG_SANDBOX_MAX_ARGV) {
        status = SPG_E_OVERFLOW;
    }
    if (status != SPG_OK) {
        *out = (struct spg_sandbox_wrapper){};
    }
    return status;
}
