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
    if (spg_chat_tool_dispatch(&store, nullptr, false, strlen(text), text, sizeof out, out,
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
    if (spg_chat_tool_dispatch(&store, nullptr, false, strlen(save), save, sizeof out, out,
                               &was_tool) != SPG_OK ||
        !was_tool || strstr(out, "saved fav") == nullptr) {
        return 1;
    }

    const char read[] = "(tool memory_read (slug \"fav\"))";
    if (spg_chat_tool_dispatch(&store, nullptr, false, strlen(read), read, sizeof out, out,
                               &was_tool) != SPG_OK ||
        !was_tool || strstr(out, "the body content") == nullptr) {
        return 1;
    }

    const char list[] = "(tool memory_list)";
    if (spg_chat_tool_dispatch(&store, nullptr, false, strlen(list), list, sizeof out, out,
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
    if (spg_chat_tool_dispatch(&store, nullptr, false, strlen(del), del, sizeof out, out,
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
    if (spg_chat_tool_dispatch(&store, nullptr, false, strlen(unk), unk, sizeof out, out,
                               &was_tool) != SPG_OK ||
        !was_tool || strstr(out, "unknown tool") == nullptr) {
        return 1;
    }
    const char bad[] = "(tool memory_read)"; /* missing slug */
    if (spg_chat_tool_dispatch(&store, nullptr, false, strlen(bad), bad, sizeof out, out,
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
    if (spg_chat_tool_dispatch(&store, nullptr, false, strlen(ex), ex, sizeof out, out,
                               &was_tool) != SPG_OK ||
        !was_tool || strstr(out, "disabled") == nullptr) {
        return 1;
    }
    /* allowed -> runs the command, output captured */
    if (spg_chat_tool_dispatch(&store, nullptr, true, strlen(ex), ex, sizeof out, out,
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
    const bool ok = spg_chat_tool_dispatch(&store, &journal, false, strlen(save),
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

int main(void) {
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
