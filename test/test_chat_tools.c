#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#    define _DARWIN_C_SOURCE 1
#endif

#include "geistshell/chat_tools.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int open_temp(struct spg_mem_store *store, char dir[static 64]) {
    memcpy(dir, "/tmp/spg_chattool_XXXXXX", 25u);
    if (mkdtemp(dir) == nullptr) {
        return 1;
    }
    return spg_mem_store_open(store, dir) == SPG_OK ? 0 : 1;
}

static int test_not_a_tool(void) {
    struct spg_mem_store store;
    char                 dir[64];
    if (open_temp(&store, dir) != 0) {
        return 1;
    }
    char out[64];
    bool was_tool = true;
    const char text[] = "Just a normal reply, not a tool call.";
    if (spg_chat_tool_dispatch(&store, nullptr, false, nullptr, strlen(text), text, sizeof out, out,
                               &was_tool) != SPG_OK) {
        return 1;
    }
    return (!was_tool && out[0] == '\0') ? 0 : 1;
}

static int test_save_then_read(void) {
    struct spg_mem_store store;
    char                 dir[64];
    if (open_temp(&store, dir) != 0) {
        return 1;
    }
    char out[1024];
    bool was_tool = false;

    const char save[] =
        "(tool memory_save (slug \"fav\") (description \"a fav\") "
        "(body \"the body content\"))";
    if (spg_chat_tool_dispatch(&store, nullptr, false, nullptr, strlen(save), save, sizeof out, out,
                               &was_tool) != SPG_OK ||
        !was_tool || strstr(out, "saved fav") == nullptr) {
        return 1;
    }

    const char read[] = "(tool memory_read (slug \"fav\"))";
    if (spg_chat_tool_dispatch(&store, nullptr, false, nullptr, strlen(read), read, sizeof out, out,
                               &was_tool) != SPG_OK ||
        !was_tool || strstr(out, "the body content") == nullptr) {
        return 1;
    }

    const char list[] = "(tool memory_list)";
    if (spg_chat_tool_dispatch(&store, nullptr, false, nullptr, strlen(list), list, sizeof out, out,
                               &was_tool) != SPG_OK ||
        !was_tool || strstr(out, "fav: a fav") == nullptr) {
        return 1;
    }
    return 0;
}

static int test_delete(void) {
    struct spg_mem_store store;
    char                 dir[64];
    if (open_temp(&store, dir) != 0) {
        return 1;
    }
    if (spg_mem_save(&store, "gone", "d", "b") != SPG_OK) {
        return 1;
    }
    char out[256];
    bool was_tool = false;
    const char del[] = "(tool memory_delete (slug \"gone\"))";
    if (spg_chat_tool_dispatch(&store, nullptr, false, nullptr, strlen(del), del, sizeof out, out,
                               &was_tool) != SPG_OK ||
        !was_tool || strstr(out, "deleted gone") == nullptr) {
        return 1;
    }
    char body[64];
    return spg_mem_read(&store, "gone", sizeof body, body, nullptr) ==
                   SPG_E_NOT_FOUND
               ? 0
               : 1;
}

static int test_unknown_and_bad_args(void) {
    struct spg_mem_store store;
    char                 dir[64];
    if (open_temp(&store, dir) != 0) {
        return 1;
    }
    char out[128];
    bool was_tool = false;
    const char unk[] = "(tool memory_frobnicate (slug \"x\"))";
    if (spg_chat_tool_dispatch(&store, nullptr, false, nullptr, strlen(unk), unk, sizeof out, out,
                               &was_tool) != SPG_OK ||
        !was_tool || strstr(out, "unknown tool") == nullptr) {
        return 1;
    }
    const char bad[] = "(tool memory_read)"; /* missing slug */
    if (spg_chat_tool_dispatch(&store, nullptr, false, nullptr, strlen(bad), bad, sizeof out, out,
                               &was_tool) != SPG_OK ||
        !was_tool || strstr(out, "error") == nullptr) {
        return 1;
    }
    return 0;
}

static int test_exec_gated(void) {
    struct spg_mem_store store;
    char                 dir[64];
    if (open_temp(&store, dir) != 0) {
        return 1;
    }
    char out[1024];
    bool was_tool = false;
    const char ex[] = "(tool exec (command \"echo chat-exec-ok\"))";

    /* disabled by default */
    if (spg_chat_tool_dispatch(&store, nullptr, false, nullptr, strlen(ex), ex, sizeof out, out,
                               &was_tool) != SPG_OK ||
        !was_tool || strstr(out, "disabled") == nullptr) {
        return 1;
    }
    /* allowed -> runs the command, output captured */
    if (spg_chat_tool_dispatch(&store, nullptr, true, nullptr, strlen(ex), ex, sizeof out, out,
                               &was_tool) != SPG_OK ||
        !was_tool || strstr(out, "chat-exec-ok") == nullptr ||
        strstr(out, "exit 0") == nullptr) {
        return 1;
    }
    return 0;
}

static int test_journaled_save(void) {
    struct spg_mem_store store;
    char                 dir[64];
    if (open_temp(&store, dir) != 0) {
        return 1;
    }
    char jpath[96];
    (void)snprintf(jpath, sizeof jpath, "%s/j.sgj", dir);
    struct spg_journal_writer journal = {};
    if (spg_journal_writer_open(&journal, jpath) != SPG_OK) {
        return 1;
    }
    char       out[256];
    bool       was_tool = false;
    const char save[] =
        "(tool memory_save (slug \"j\") (description \"d\") (body \"b\"))";
    const bool ok = spg_chat_tool_dispatch(&store, &journal, false, nullptr, strlen(save),
                                           save, sizeof out, out, &was_tool) ==
                        SPG_OK &&
                    was_tool && strstr(out, "saved j") != nullptr;
    (void)spg_journal_writer_close(&journal);
    if (!ok) {
        return 1;
    }
    /* The governed save was journaled: read back and count memory events. */
    struct spg_journal_reader reader = {};
    if (spg_journal_reader_open(&reader, jpath) != SPG_OK) {
        return 1;
    }
    size_t mem_events = 0u;
    for (;;) {
        uint8_t                   payload[1024];
        struct spg_journal_record rec = {};
        if (spg_journal_reader_next(&reader, sizeof payload, payload, &rec) !=
            SPG_OK) {
            break;
        }
        if (rec.header.event_kind == (uint32_t)SPG_JOURNAL_EVENT_MEMORY) {
            mem_events += 1u;
        }
    }
    (void)spg_journal_reader_close(&reader);
    return mem_events >= 1u ? 0 : 1;
}

/* The agent_run gate: no launcher = unavailable, a declining operator is
 * final, and a confirmed run execs argv-style (no shell, nothing to inject).
 * /usr/bin/true stands in for the geistshell binary: this test proves the
 * gate, not the agent. */
static bool confirm_no(void *userdata, const char *config_path) {
    (void)config_path;
    *(int *)userdata += 1;
    return false;
}

static bool confirm_yes(void *userdata, const char *config_path) {
    (void)config_path;
    *(int *)userdata += 1;
    return true;
}

static int test_agent_run_gate(void) {
    struct spg_mem_store store;
    char                 dir[64];
    if (open_temp(&store, dir) != 0) {
        return 1;
    }
    char cfg[96];
    (void)snprintf(cfg, sizeof cfg, "%s/run.spg", dir);
    FILE *f = fopen(cfg, "wb");
    if (f == nullptr) {
        return 1;
    }
    (void)fputs("(run)", f);
    (void)fclose(f);

    char call[160];
    (void)snprintf(call, sizeof call, "(tool agent_run (config \"%s\"))", cfg);
    char out[256];
    bool was_tool = false;

    /* No launcher: the tool exists but reports itself unavailable. */
    if (spg_chat_tool_dispatch(&store, nullptr, false, nullptr, strlen(call),
                               call, sizeof out, out, &was_tool) != SPG_OK ||
        !was_tool || strstr(out, "not available") == nullptr) {
        return 1;
    }

    /* Declined: confirm was asked exactly once, nothing ran. */
    int                                  asked    = 0;
    struct spg_chat_agent_launcher launcher = {.agent_bin = "/usr/bin/true",
                                               .confirm   = confirm_no,
                                               .userdata  = &asked};
    if (spg_chat_tool_dispatch(&store, nullptr, false, &launcher, strlen(call),
                               call, sizeof out, out, &was_tool) != SPG_OK ||
        strstr(out, "declined") == nullptr || asked != 1) {
        return 1;
    }

    /* A config that does not exist is refused BEFORE the operator is asked —
     * no confirmation dialog for a run that could never start. */
    asked = 0;
    const char missing[] = "(tool agent_run (config \"/nonexistent.spg\"))";
    if (spg_chat_tool_dispatch(&store, nullptr, false, &launcher,
                               strlen(missing), missing, sizeof out, out,
                               &was_tool) != SPG_OK ||
        strstr(out, "cannot read config") == nullptr || asked != 0) {
        return 1;
    }

    /* Confirmed: the exec happens and its exit code comes back. */
    asked            = 0;
    launcher.confirm = confirm_yes;
    if (spg_chat_tool_dispatch(&store, nullptr, false, &launcher, strlen(call),
                               call, sizeof out, out, &was_tool) != SPG_OK ||
        strstr(out, "exit 0") == nullptr || asked != 1) {
        (void)fprintf(stderr, "  agent_run: %s\n", out);
        return 1;
    }
    return 0;
}

int main(void) {
    if (test_agent_run_gate() != 0) {
        fprintf(stderr, "test_agent_run_gate failed\n");
        return 1;
    }
    if (test_not_a_tool() != 0) {
        fprintf(stderr, "test_not_a_tool failed\n");
        return 1;
    }
    if (test_journaled_save() != 0) {
        fprintf(stderr, "test_journaled_save failed\n");
        return 1;
    }
    if (test_save_then_read() != 0) {
        fprintf(stderr, "test_save_then_read failed\n");
        return 1;
    }
    if (test_delete() != 0) {
        fprintf(stderr, "test_delete failed\n");
        return 1;
    }
    if (test_unknown_and_bad_args() != 0) {
        fprintf(stderr, "test_unknown_and_bad_args failed\n");
        return 1;
    }
    if (test_exec_gated() != 0) {
        fprintf(stderr, "test_exec_gated failed\n");
        return 1;
    }
    return 0;
}
