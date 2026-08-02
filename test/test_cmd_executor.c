#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#    define _DARWIN_C_SOURCE 1
#endif

#include "geistshell/cmd_executor.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ms(void) {
    struct timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static struct spg_cmd_request make_req(const char *const *argv, size_t argc,
                                       uint64_t timeout_ms, char *out,
                                       size_t out_cap, char *err,
                                       size_t err_cap) {
    return (struct spg_cmd_request){
        .argc       = argc,
        .argv       = argv,
        .timeout_ms = timeout_ms,
        .stdout_cap = out_cap,
        .stdout_buf = out,
        .stderr_cap = err_cap,
        .stderr_buf = err,
    };
}

static int test_echo_stdout(void) {
    const char *const argv[] = {"echo", "hello"};
    char              out[64] = {0};
    char              err[64] = {0};
    struct spg_cmd_request req =
        make_req(argv, 2u, 2000u, out, sizeof out, err, sizeof err);
    struct spg_cmd_result res = {};

    if (spg_cmd_executor_run(1u, &req, &res) != SPG_OK) {
        return 1;
    }
    if (res.status != SPG_OK || !res.started || !res.exited ||
        res.exit_code != 0 || res.timed_out) {
        return 1;
    }
    /* echo appends a newline. */
    if (strstr(out, "hello") == nullptr || res.stdout_len == 0u) {
        return 1;
    }
    return 0;
}

static int test_nonzero_exit(void) {
    const char *const argv[] = {"false"};
    char              out[16] = {0};
    char              err[16] = {0};
    struct spg_cmd_request req =
        make_req(argv, 1u, 2000u, out, sizeof out, err, sizeof err);
    struct spg_cmd_result res = {};

    if (spg_cmd_executor_run(1u, &req, &res) != SPG_OK) {
        return 1;
    }
    return (res.started && res.exited && res.exit_code == 1) ? 0 : 1;
}

static int test_stdout_truncation(void) {
    /* printf prints its literal first arg (no % specifiers): 10 bytes. */
    const char *const argv[] = {"printf", "ABCDEFGHIJ"};
    char              out[4] = {0};
    char              err[16] = {0};
    struct spg_cmd_request req =
        make_req(argv, 2u, 2000u, out, sizeof out, err, sizeof err);
    struct spg_cmd_result res = {};

    if (spg_cmd_executor_run(1u, &req, &res) != SPG_OK) {
        return 1;
    }
    /* cap 4 keeps 3 bytes + terminator, flags truncation. */
    if (res.stdout_len != 3u || !res.stdout_truncated ||
        strcmp(out, "ABC") != 0) {
        return 1;
    }
    return 0;
}

static int test_timeout_kills(void) {
    const char *const argv[] = {"sleep", "5"};
    char              out[16] = {0};
    char              err[16] = {0};
    struct spg_cmd_request req =
        make_req(argv, 2u, 150u, out, sizeof out, err, sizeof err);
    struct spg_cmd_result res = {};

    const uint64_t t0 = now_ms();
    if (spg_cmd_executor_run(1u, &req, &res) != SPG_OK) {
        return 1;
    }
    const uint64_t elapsed = now_ms() - t0;
    /* Killed well before the 5s sleep would finish. */
    if (!res.started || !res.timed_out || res.exited || elapsed > 2000u) {
        return 1;
    }
    return 0;
}

static int test_concurrent_batch(void) {
    /* Three 1s sleeps run together finish in ~1s, not ~3s. */
    const char *const a0[] = {"sleep", "1"};
    const char *const a1[] = {"sleep", "1"};
    const char *const a2[] = {"sleep", "1"};
    char              ob[3][8] = {{0}, {0}, {0}};
    char              eb[3][8] = {{0}, {0}, {0}};
    struct spg_cmd_request reqs[3] = {
        make_req(a0, 2u, 5000u, ob[0], sizeof ob[0], eb[0], sizeof eb[0]),
        make_req(a1, 2u, 5000u, ob[1], sizeof ob[1], eb[1], sizeof eb[1]),
        make_req(a2, 2u, 5000u, ob[2], sizeof ob[2], eb[2], sizeof eb[2]),
    };
    struct spg_cmd_result res[3] = {};

    const uint64_t t0 = now_ms();
    if (spg_cmd_executor_run(3u, reqs, res) != SPG_OK) {
        return 1;
    }
    const uint64_t elapsed = now_ms() - t0;
    for (size_t i = 0u; i < 3u; i += 1u) {
        if (!res[i].started || !res[i].exited || res[i].exit_code != 0) {
            return 1;
        }
    }
    /* Serial would be ~3000ms; concurrent must be well under. */
    return elapsed < 2500u ? 0 : 1;
}

static int test_command_not_found(void) {
    const char *const argv[] = {"definitely_not_a_real_binary_xyz"};
    char              out[16] = {0};
    char              err[16] = {0};
    struct spg_cmd_request req =
        make_req(argv, 1u, 2000u, out, sizeof out, err, sizeof err);
    struct spg_cmd_result res = {};

    if (spg_cmd_executor_run(1u, &req, &res) != SPG_OK) {
        return 1;
    }
    return (!res.started && res.status == SPG_E_NOT_FOUND) ? 0 : 1;
}

static int test_invalid_args(void) {
    char                   out[8] = {0};
    char                   err[8] = {0};
    const char *const      argv[] = {"echo"};
    struct spg_cmd_request req =
        make_req(argv, 1u, 0u, out, sizeof out, err, sizeof err);
    struct spg_cmd_result res = {};

    /* Null arrays rejected; zero count is a no-op; oversize batch rejected. */
    if (spg_cmd_executor_run(1u, nullptr, &res) != SPG_E_INVALID_ARG ||
        spg_cmd_executor_run(1u, &req, nullptr) != SPG_E_INVALID_ARG) {
        return 1;
    }
    if (spg_cmd_executor_run(0u, &req, &res) != SPG_OK) {
        return 1;
    }

    struct spg_cmd_request big[SPG_CMD_MAX_BATCH + 1u] = {};
    struct spg_cmd_result  bigres[SPG_CMD_MAX_BATCH + 1u] = {};
    if (spg_cmd_executor_run(SPG_CMD_MAX_BATCH + 1u, big, bigres) !=
        SPG_E_LIMIT) {
        return 1;
    }

    /* A per-request invalid (argc 0) is reported per-result, batch is OK. */
    struct spg_cmd_request bad =
        make_req(argv, 0u, 0u, out, sizeof out, err, sizeof err);
    struct spg_cmd_result badres = {};
    if (spg_cmd_executor_run(1u, &bad, &badres) != SPG_OK) {
        return 1;
    }
    return (!badres.started && badres.status == SPG_E_INVALID_ARG) ? 0 : 1;
}

static int test_file_size_limit(void) {
    char tmpl[] = "/tmp/spg_fsz_XXXXXX";
    const int fd = mkstemp(tmpl);
    if (fd < 0) {
        return 1;
    }
    (void)close(fd);
    char ofarg[80];
    (void)snprintf(ofarg, sizeof ofarg, "of=%s", tmpl);
    /* dd would write 10 MiB, but the file-size cap is 64 KiB -> SIGXFSZ. */
    const char *const argv[] = {"dd", "if=/dev/zero", ofarg, "bs=1048576",
                                "count=10"};
    char                   out[16] = {0};
    char                   err[256] = {0};
    struct spg_cmd_request req =
        make_req(argv, 5u, 5000u, out, sizeof out, err, sizeof err);
    req.limits.file_bytes = 65536u;
    struct spg_cmd_result res = {};
    const enum spg_status rc = spg_cmd_executor_run(1u, &req, &res);
    (void)unlink(tmpl);
    if (rc != SPG_OK) {
        return 1;
    }
    /* The child started, was not a normal exit, and died from the file-size
     * signal. */
    return (res.started && !res.exited && res.term_signal == SIGXFSZ) ? 0 : 1;
}

static int test_clear_env(void) {
    const char *const argv[] = {"env"};
    char              out[256] = {0};
    char              err[64] = {0};
    struct spg_cmd_request req =
        make_req(argv, 1u, 2000u, out, sizeof out, err, sizeof err);
    req.clear_env = true;
    struct spg_cmd_result res = {};
    if (spg_cmd_executor_run(1u, &req, &res) != SPG_OK) {
        return 1;
    }
    /* env was resolved via PATH but ran with an empty environment: no output. */
    return (res.started && res.exited && res.exit_code == 0 &&
            res.stdout_len == 0u)
               ? 0
               : 1;
}

int main(void) {
    if (test_file_size_limit() != 0) {
        fprintf(stderr, "test_file_size_limit failed\n");
        return 1;
    }
    if (test_clear_env() != 0) {
        fprintf(stderr, "test_clear_env failed\n");
        return 1;
    }
    if (test_echo_stdout() != 0) {
        fprintf(stderr, "test_echo_stdout failed\n");
        return 1;
    }
    if (test_nonzero_exit() != 0) {
        fprintf(stderr, "test_nonzero_exit failed\n");
        return 1;
    }
    if (test_stdout_truncation() != 0) {
        fprintf(stderr, "test_stdout_truncation failed\n");
        return 1;
    }
    if (test_timeout_kills() != 0) {
        fprintf(stderr, "test_timeout_kills failed\n");
        return 1;
    }
    if (test_concurrent_batch() != 0) {
        fprintf(stderr, "test_concurrent_batch failed\n");
        return 1;
    }
    if (test_command_not_found() != 0) {
        fprintf(stderr, "test_command_not_found failed\n");
        return 1;
    }
    if (test_invalid_args() != 0) {
        fprintf(stderr, "test_invalid_args failed\n");
        return 1;
    }
    return 0;
}
