#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#    define _DARWIN_C_SOURCE 1
#endif

#include "geistshell/exec_command.h"

#include "geistshell/cmd_executor.h"
#include "geistshell/cmd_menu.h"
#include "geistshell/executor_boundary.h"
#include "geistshell/host_probe.h"
#include "geistshell/policy.h"
#include "geistshell/recommendation.h"
#include "geistshell/sexpr.h"
#include "geistshell/status.h"

#include <stdio.h>
#include <string.h>

#define EXEC_OUTPUT_CAP   32768u
#define EXEC_PAYLOAD_CAP  4096u
#define EXEC_MAX_TIMEOUT  5000u
#define EXEC_DEF_TIMEOUT  3000u
#define EXEC_MAX_OUT_BYTE (1u << 20)

/* Append a double-quoted, escaped string so values with spaces or parens stay
 * a single s-expression token. */
static void append_quoted(struct spg_sexpr_writer *w, const char *s) {
    (void)spg_sexpr_writer_append_text(w, "\"");
    for (const char *p = s; *p != '\0'; p += 1u) {
        const char c = *p;
        if (c == '"' || c == '\\') {
            const char esc[3] = {'\\', c, '\0'};
            (void)spg_sexpr_writer_append_text(w, esc);
        } else if (c == '\n' || c == '\r' || c == '\t') {
            (void)spg_sexpr_writer_append_text(w, " ");
        } else {
            const char one[2] = {c, '\0'};
            (void)spg_sexpr_writer_append_text(w, one);
        }
    }
    (void)spg_sexpr_writer_append_text(w, "\"");
}

static void render_field(struct spg_sexpr_writer *w, const char *key,
                         const char *value) {
    (void)spg_sexpr_writer_append_text(w, " (");
    (void)spg_sexpr_writer_append_text(w, key);
    (void)spg_sexpr_writer_append_text(w, " ");
    append_quoted(w, value);
    (void)spg_sexpr_writer_append_text(w, ")");
}

static void render_bool(struct spg_sexpr_writer *w, const char *key,
                        const bool value) {
    (void)spg_sexpr_writer_append_text(w, " (");
    (void)spg_sexpr_writer_append_text(w, key);
    (void)spg_sexpr_writer_append_text(w, value ? " true)" : " false)");
}

static void render_u64(struct spg_sexpr_writer *w, const char *key,
                       const uint64_t value) {
    (void)spg_sexpr_writer_append_text(w, " (");
    (void)spg_sexpr_writer_append_text(w, key);
    (void)spg_sexpr_writer_append_text(w, " ");
    (void)spg_sexpr_writer_append_u64(w, value);
    (void)spg_sexpr_writer_append_text(w, ")");
}

static void render_host(struct spg_sexpr_writer *w,
                        const struct spg_host_info *host) {
    (void)spg_sexpr_writer_append_text(w, " (host (os ");
    (void)spg_sexpr_writer_append_text(w, spg_host_os_to_string(host->os));
    (void)spg_sexpr_writer_append_text(w, ")");
    render_field(w, "sysname", host->sysname);
    render_field(w, "release", host->release);
    render_field(w, "version", host->version);
    render_field(w, "machine", host->machine);
    render_field(w, "nodename", host->nodename);
    (void)spg_sexpr_writer_append_text(w, ")");
}

int spg_exec_command(const int argc, char **argv) {
    if (argc < 1 || argv == nullptr || argv[0] == nullptr ||
        argv[0][0] == '\0') {
        fprintf(stderr, "usage: geistshell exec <command> [args...]\n");
        return 2;
    }

    struct spg_host_info host = {};
    (void)spg_host_probe(&host); /* unknown host is fine; fields stay empty */

    const struct spg_cmd_menu_entry *desc = spg_cmd_menu_find(argv[0]);
    const bool     known        = desc != nullptr;
    const bool     uses_network = known && desc->uses_network;
    uint64_t       timeout_ms   = EXEC_DEF_TIMEOUT;
    if (known && desc->default_timeout_ms > 0u) {
        timeout_ms = desc->default_timeout_ms < EXEC_MAX_TIMEOUT
                         ? desc->default_timeout_ms
                         : EXEC_MAX_TIMEOUT;
    }

    /* Gate the free-form shell command through the shared boundary. */
    const struct spg_executor_boundary_config bcfg = {
        .execution_enabled     = true,
        .allowed_workdir_prefix = "/",
        .max_timeout_ms        = EXEC_MAX_TIMEOUT,
        .max_stdout_bytes      = EXEC_MAX_OUT_BYTE,
        .max_stderr_bytes      = EXEC_MAX_OUT_BYTE,
        .require_clean_env     = false,
    };
    const struct spg_executor_boundary_request breq = {
        .working_dir        = "/",
        .timeout_ms         = timeout_ms,
        .stdout_limit_bytes = EXEC_OUTPUT_CAP,
        .stderr_limit_bytes = EXEC_OUTPUT_CAP,
        .env_cleared        = false,
    };
    struct spg_executor_boundary_plan plan = {};
    if (spg_executor_boundary_check_shell(&bcfg, argv[0], uses_network, &breq,
                                          &plan) != SPG_OK) {
        fprintf(stderr, "exec: internal boundary error\n");
        return 4;
    }

    char                    payload[EXEC_PAYLOAD_CAP];
    struct spg_sexpr_writer w;
    spg_sexpr_writer_init(&w, sizeof payload, payload);
    (void)spg_sexpr_writer_append_text(&w, "(exec_result");
    render_host(&w, &host);
    (void)spg_sexpr_writer_append_text(&w, " (command");
    render_field(&w, "name", argv[0]);
    render_bool(&w, "known", known);
    render_u64(&w, "argc", (uint64_t)argc);
    (void)spg_sexpr_writer_append_text(&w, ")");
    (void)spg_sexpr_writer_append_text(&w, " (boundary");
    render_bool(&w, "approved", plan.approved);
    render_field(&w, "reason",
                 spg_executor_boundary_reason_to_string(plan.reason));
    (void)spg_sexpr_writer_append_text(&w, ")");

    if (!plan.approved) {
        (void)spg_sexpr_writer_append_text(&w, ")");
        printf("%s\n", payload);
        return 3;
    }

    char stdout_buf[EXEC_OUTPUT_CAP];
    char stderr_buf[EXEC_OUTPUT_CAP];
    const struct spg_cmd_request req = {
        .argc       = (size_t)argc,
        .argv       = (const char *const *)argv,
        .working_dir = nullptr,
        .timeout_ms = timeout_ms,
        .clear_env  = false,
        .limits     = SPG_CMD_DEFAULT_LIMITS,
        .stdout_cap = sizeof stdout_buf,
        .stdout_buf = stdout_buf,
        .stderr_cap = sizeof stderr_buf,
        .stderr_buf = stderr_buf,
    };
    struct spg_cmd_result result = {};
    const enum spg_status run_status =
        spg_cmd_executor_run(1u, &req, &result);

    render_field(&w, "status", spg_status_to_string(result.status));
    render_bool(&w, "started", result.started);
    render_bool(&w, "exited", result.exited);
    render_u64(&w, "exit_code", (uint64_t)(result.exit_code & 0xff));
    render_u64(&w, "term_signal", (uint64_t)result.term_signal);
    render_bool(&w, "timed_out", result.timed_out);
    render_u64(&w, "stdout_len", result.stdout_len);
    render_bool(&w, "stdout_truncated", result.stdout_truncated);
    render_u64(&w, "stderr_len", result.stderr_len);
    render_bool(&w, "stderr_truncated", result.stderr_truncated);
    (void)spg_sexpr_writer_append_text(&w, ")");

    printf("%s\n", payload);
    if (result.stdout_len > 0u) {
        printf("--- stdout ---\n%s\n", stdout_buf);
    }
    if (result.stderr_len > 0u) {
        printf("--- stderr ---\n%s\n", stderr_buf);
    }

    if (run_status != SPG_OK || result.status != SPG_OK || !result.started) {
        return 4;
    }
    if (result.timed_out || !result.exited) {
        return 4;
    }
    return result.exit_code & 0xff;
}
