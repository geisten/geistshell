#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#    define _DARWIN_C_SOURCE 1
#endif

#include "geistshell/cmd_executor.h"
#include "geistshell/sandbox.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool has_arg(const struct spg_sandbox_wrapper *w, const char *needle) {
    for (size_t i = 0u; i < w->argc; i += 1u) {
        if (strcmp(w->argv[i], needle) == 0) {
            return true;
        }
    }
    return false;
}

/* The bwrap prefix must unshare everything (network included) and open exactly
 * one writable path. A missing --unshare-all or a stray --share-net is the
 * whole sandbox gone, so the shape is asserted token by token. */
static int test_bwrap_shape(void) {
    const struct spg_sandbox_spec spec = {
        .enabled = true, .allow_network = false, .rw_dir = "/tmp/work"};
    struct spg_sandbox_wrapper w = {};
    if (spg_sandbox_wrapper_build("/usr/bin/bwrap", &spec, &w) != SPG_OK) {
        return 1;
    }
    if (w.argc < 2u || strcmp(w.argv[0], "/usr/bin/bwrap") != 0 ||
        strcmp(w.argv[w.argc - 1u], "--") != 0) {
        return 1;
    }
    if (!has_arg(&w, "--unshare-all") || !has_arg(&w, "--die-with-parent") ||
        !has_arg(&w, "--ro-bind") || !has_arg(&w, "--tmpfs") ||
        !has_arg(&w, "--bind") || !has_arg(&w, "/tmp/work")) {
        return 1;
    }
    if (has_arg(&w, "--share-net")) {
        return 1;
    }

    const struct spg_sandbox_spec net = {
        .enabled = true, .allow_network = true, .rw_dir = nullptr};
    struct spg_sandbox_wrapper nw = {};
    if (spg_sandbox_wrapper_build("/usr/bin/bwrap", &net, &nw) != SPG_OK ||
        !has_arg(&nw, "--share-net") || has_arg(&nw, "--bind")) {
        return 1;
    }
    return 0;
}

static int test_seatbelt_shape(void) {
    const struct spg_sandbox_spec spec = {
        .enabled = true, .allow_network = false, .rw_dir = "/private/tmp/work"};
    struct spg_sandbox_wrapper w = {};
    if (spg_sandbox_wrapper_build("/usr/bin/sandbox-exec", &spec, &w) !=
        SPG_OK) {
        return 1;
    }
    if (w.argc != 4u || strcmp(w.argv[1], "-p") != 0 ||
        w.argv[2] != w.profile || strcmp(w.argv[3], "--") != 0) {
        return 1;
    }
    if (strstr(w.profile, "(deny network*)") == nullptr ||
        strstr(w.profile, "(deny file-write*)") == nullptr ||
        strstr(w.profile, "/private/tmp/work") == nullptr) {
        return 1;
    }

    const struct spg_sandbox_spec net = {
        .enabled = true, .allow_network = true, .rw_dir = nullptr};
    struct spg_sandbox_wrapper nw = {};
    if (spg_sandbox_wrapper_build("/usr/bin/sandbox-exec", &net, &nw) !=
            SPG_OK ||
        strstr(nw.profile, "(deny network*)") != nullptr) {
        return 1;
    }
    return 0;
}

/* rw_dir lands inside a quoted SBPL string and in a bwrap bind argument. A
 * relative path would isolate the wrong directory; a quote would end the
 * string early and let the rest of the path write the profile. */
static int test_rejects_bad_input(void) {
    struct spg_sandbox_wrapper w = {};
    const struct spg_sandbox_spec relative = {.enabled = true,
                                              .rw_dir  = "work"};
    const struct spg_sandbox_spec quoted = {
        .enabled = true, .rw_dir = "/tmp/a\") (subpath \"/"};
    const struct spg_sandbox_spec disabled = {.enabled = false,
                                              .rw_dir  = "/tmp"};
    const struct spg_sandbox_spec ok = {.enabled = true, .rw_dir = "/tmp"};

    if (spg_sandbox_wrapper_build("/usr/bin/bwrap", &relative, &w) !=
            SPG_E_INVALID_ARG ||
        spg_sandbox_wrapper_build("/usr/bin/sandbox-exec", &quoted, &w) !=
            SPG_E_INVALID_ARG ||
        spg_sandbox_wrapper_build("/usr/bin/bwrap", &disabled, &w) !=
            SPG_E_INVALID_ARG ||
        spg_sandbox_wrapper_build("/usr/bin/firejail", &ok, &w) !=
            SPG_E_UNSUPPORTED ||
        spg_sandbox_wrapper_build(nullptr, &ok, &w) != SPG_E_INVALID_ARG) {
        return 1;
    }
    /* A rejected build leaves nothing runnable behind. */
    return w.argc == 0u ? 0 : 1;
}

/* Fail closed: an enabled spec on a host with no sandbox tool must not fall
 * back to running the command. Only assertable where there IS no tool. */
static int test_fails_closed(void) {
    if (spg_sandbox_tool_path() != nullptr) {
        return 0;
    }
    const char *const      argv[]  = {"echo", "escaped"};
    char                   out[64] = {0};
    char                   err[64] = {0};
    struct spg_cmd_request req     = {
            .argc       = 2u,
            .argv       = argv,
            .timeout_ms = 2000u,
            .sandbox    = {.enabled = true},
            .stdout_cap = sizeof out,
            .stdout_buf = out,
            .stderr_cap = sizeof err,
            .stderr_buf = err,
    };
    struct spg_cmd_result res = {};
    if (spg_cmd_executor_run(1u, &req, &res) != SPG_OK) {
        return 1;
    }
    return (!res.started && res.status == SPG_E_UNSUPPORTED) ? 0 : 1;
}

/* The one that matters: a real command, really confined. Writing inside the
 * sandbox's directory works; writing next to the running test does not.
 * (Network denial is not exercised here -- it would need an egress target and
 * a timeout budget; the --unshare-net / (deny network*) token is asserted in
 * the shape tests above.) */
static int test_enforced_write_boundary(const char *tmpdir) {
    char inside[512];
    char outside[512];
    char cwd[256];
    if (getcwd(cwd, sizeof cwd) == nullptr) {
        return 1;
    }
    (void)snprintf(inside, sizeof inside, "%s/inside.tmp", tmpdir);
    (void)snprintf(outside, sizeof outside, "%s/spg_sandbox_escape.tmp", cwd);
    (void)unlink(inside);
    (void)unlink(outside);

    const char *const in_argv[]  = {"touch", inside};
    const char *const out_argv[] = {"touch", outside};
    char              o1[256]    = {0};
    char              e1[256]    = {0};
    char              o2[256]    = {0};
    char              e2[256]    = {0};
    const struct spg_sandbox_spec spec = {
        .enabled = true, .allow_network = false, .rw_dir = tmpdir};
    struct spg_cmd_request reqs[2] = {
        {.argc       = 2u,
         .argv       = in_argv,
         .timeout_ms = 5000u,
         .sandbox    = spec,
         .stdout_cap = sizeof o1,
         .stdout_buf = o1,
         .stderr_cap = sizeof e1,
         .stderr_buf = e1},
        {.argc       = 2u,
         .argv       = out_argv,
         .timeout_ms = 5000u,
         .sandbox    = spec,
         .stdout_cap = sizeof o2,
         .stdout_buf = o2,
         .stderr_cap = sizeof e2,
         .stderr_buf = e2},
    };
    struct spg_cmd_result res[2] = {};
    if (spg_cmd_executor_run(2u, reqs, res) != SPG_OK) {
        return 1;
    }

    int rc = 0;
    if (!res[0].started || !res[0].exited || res[0].exit_code != 0 ||
        access(inside, F_OK) != 0) {
        (void)fprintf(stderr,
                      "sandbox blocked its own writable dir (exit %d): %s\n",
                      res[0].exit_code, e1);
        rc = 1;
    }
    if (access(outside, F_OK) == 0) {
        (void)fprintf(stderr, "sandbox escape: %s was created\n", outside);
        (void)unlink(outside);
        rc = 1;
    }
    if (res[1].exited && res[1].exit_code == 0) {
        (void)fprintf(stderr, "write outside the sandbox reported success\n");
        rc = 1;
    }
    (void)unlink(inside);
    return rc;
}

int main(void) {
    if (test_bwrap_shape() != 0) {
        (void)fprintf(stderr, "test_bwrap_shape failed\n");
        return 1;
    }
    if (test_seatbelt_shape() != 0) {
        (void)fprintf(stderr, "test_seatbelt_shape failed\n");
        return 1;
    }
    if (test_rejects_bad_input() != 0) {
        (void)fprintf(stderr, "test_rejects_bad_input failed\n");
        return 1;
    }
    if (test_fails_closed() != 0) {
        (void)fprintf(stderr, "test_fails_closed failed\n");
        return 1;
    }

    if (spg_sandbox_tool_path() == nullptr) {
        /* SKIP(host), not SKIP: every platform geistshell targets has one of
         * the two tools, so an absent sandbox means governed execution is dead
         * on this host -- CI must see that, not count it as covered. */
        (void)printf(
            "test_sandbox: SKIP(host) (no bwrap/sandbox-exec on this host)\n");
        return 0;
    }
    /* Deliberately NOT under /tmp: the macOS profile allows /private/tmp
     * outright, so a temp dir there would pass without the rw_dir ever being
     * honoured. Next to the test binary's cwd, only the writable bind /
     * subpath can make the write succeed. */
    char cwd[256];
    char tmpdir[320];
    if (getcwd(cwd, sizeof cwd) == nullptr) {
        (void)fprintf(stderr, "getcwd failed\n");
        return 1;
    }
    (void)snprintf(tmpdir, sizeof tmpdir, "%s/spg_sandbox_XXXXXX", cwd);
    if (mkdtemp(tmpdir) == nullptr) {
        (void)fprintf(stderr, "mkdtemp failed\n");
        return 1;
    }
    const int rc = test_enforced_write_boundary(tmpdir);
    (void)rmdir(tmpdir);
    if (rc != 0) {
        (void)fprintf(stderr, "test_enforced_write_boundary failed\n");
        return 1;
    }
    (void)printf("test_sandbox: PASS\n");
    return 0;
}
