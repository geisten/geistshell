#ifndef GEISTSHELL_CMD_EXECUTOR_H
#define GEISTSHELL_CMD_EXECUTOR_H

#include "geistshell/status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bounded concurrency: at most this many commands run in one batch. The
 * executor uses fixed stack arrays sized by these caps, so it performs no
 * dynamic allocation. */
#define SPG_CMD_MAX_BATCH 32u
#define SPG_CMD_MAX_ARGS  64u

/* OS-level resource limits applied to the child before exec (setrlimit), so a
 * runaway command cannot exhaust the host. Each field is a hard cap; 0 leaves
 * that limit at the inherited default. Enforcement is best-effort and
 * platform-dependent (e.g. RLIMIT_AS is weakly enforced on macOS) — a limit the
 * OS ignores is simply not applied. */
struct spg_cmd_limits {
    uint64_t cpu_seconds;   /* RLIMIT_CPU: max CPU seconds (SIGXCPU on exceed) */
    uint64_t address_bytes; /* RLIMIT_AS: max virtual memory */
    uint64_t file_bytes;    /* RLIMIT_FSIZE: max file written (SIGXFSZ) */
    uint64_t process_count; /* RLIMIT_NPROC: max processes for the user */
};

/* Conservative default caps for governed shell commands. CPU and file size are
 * non-breaking and always useful; address space bounds egregious memory bombs
 * (best-effort on macOS). process_count is left at 0 (a fixed RLIMIT_NPROC is
 * relative to the whole user, a footgun on busy hosts) — fork bombs are bounded
 * instead by the wall timeout plus the process-group kill. */
#define SPG_CMD_DEFAULT_LIMITS                                                  \
    ((struct spg_cmd_limits){.cpu_seconds   = 30u,                             \
                             .address_bytes = 2147483648u, /* 2 GiB */          \
                             .file_bytes    = 67108864u,   /* 64 MiB */         \
                             .process_count = 0u})

/* One command to run. The caller owns argv and the output buffers for the
 * lifetime of the call. argv holds argc entries (argv[0] is the program,
 * resolved through PATH); it need not be NUL-terminated. */
struct spg_cmd_request {
    size_t             argc;
    const char *const *argv;

    const char *working_dir; /* nullptr inherits the caller's cwd */
    uint64_t    timeout_ms;  /* 0 means no timeout */
    bool        clear_env;   /* run with an empty environment */

    struct spg_cmd_limits limits; /* OS resource caps (all 0 = inherited) */

    size_t stdout_cap; /* bytes of stdout_buf, including the terminator */
    char  *stdout_buf; /* receives captured stdout, NUL-terminated on return */
    size_t stderr_cap;
    char  *stderr_buf;
};

/* Outcome of one command. status is SPG_OK once the command was spawned and
 * reaped (inspect exited/exit_code/term_signal/timed_out for the result), or an
 * error code when the request was rejected or could not be started. */
struct spg_cmd_result {
    enum spg_status status;
    bool            started; /* the child process was spawned */
    bool            exited;  /* terminated normally (vs by signal) */
    int             exit_code;   /* valid when exited */
    int             term_signal; /* valid when started && !exited */
    bool            timed_out;   /* killed for exceeding timeout_ms */

    size_t stdout_len;
    bool   stdout_truncated;
    size_t stderr_len;
    bool   stderr_truncated;
};

/* Run n commands concurrently and fill results[i] for each. Children are
 * spawned together and their stdout/stderr are multiplexed with poll(2) into
 * the caller buffers (excess output is drained and flagged truncated so a
 * child never blocks). Per-command timeouts kill stragglers.
 *
 * Returns SPG_E_INVALID_ARG for null arrays, SPG_E_LIMIT when n exceeds
 * SPG_CMD_MAX_BATCH, and SPG_OK once the batch has been processed — individual
 * command failures are reported in results[i].status, not the return value. */
/* n may be 0 (a no-op). reqs and results are plain arrays of n elements rather
 * than [static n] so the empty batch is well-defined; both must be non-null
 * when n > 0. */
[[nodiscard]] enum spg_status
spg_cmd_executor_run(size_t n, const struct spg_cmd_request reqs[],
                     struct spg_cmd_result results[]);

/* Split s on spaces/tabs in place (writing NUL terminators), storing pointers
 * to up to argv_cap tokens in argv. No quoting or escapes. Returns the token
 * count (argc). s is mutated; argv[i] point into it. */
size_t spg_cmd_split_ws(char *s, size_t argv_cap,
                        const char *argv[static argv_cap]);

#ifdef __cplusplus
}
#endif

#endif
