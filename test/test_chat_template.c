#include "geistshell/chat_template.h"

#include <stdio.h>
#include <string.h>

static int test_single_user_turn(void) {
    char                    buf[256];
    struct spg_chat_history h;
    spg_chat_history_init(&h, sizeof buf, buf);

    if (spg_chat_history_add_user(&h, "hi") != SPG_OK) {
        return 1;
    }
    const char *expect =
        "<start_of_turn>user\nhi<end_of_turn>\n<start_of_turn>model\n";
    return strcmp(spg_chat_history_text(&h), expect) == 0 ? 0 : 1;
}

static int test_full_exchange(void) {
    char                    buf[256];
    struct spg_chat_history h;
    spg_chat_history_init(&h, sizeof buf, buf);

    if (spg_chat_history_add_user(&h, "hi") != SPG_OK ||
        spg_chat_history_add_assistant(&h, "hello") != SPG_OK ||
        spg_chat_history_add_user(&h, "bye") != SPG_OK) {
        return 1;
    }
    const char *expect =
        "<start_of_turn>user\nhi<end_of_turn>\n"
        "<start_of_turn>model\nhello<end_of_turn>\n"
        "<start_of_turn>user\nbye<end_of_turn>\n<start_of_turn>model\n";
    return strcmp(spg_chat_history_text(&h), expect) == 0 ? 0 : 1;
}

static int add_exchange(struct spg_chat_history *h, const char *u,
                        const char *a) {
    if (spg_chat_history_add_user(h, u) != SPG_OK) {
        return 1;
    }
    return spg_chat_history_add_assistant(h, a) == SPG_OK ? 0 : 1;
}

static int test_eviction(void) {
    /* Several exchanges into a small window: oldest dropped, newest kept, and
     * the buffer still starts at a user turn and stays within capacity. */
    char                    buf[200];
    struct spg_chat_history h;
    spg_chat_history_init(&h, sizeof buf, buf);

    if (add_exchange(&h, "AAA", "aaa") != 0 ||
        add_exchange(&h, "BBB", "bbb") != 0 ||
        add_exchange(&h, "CCC", "ccc") != 0 ||
        add_exchange(&h, "DDD", "ddd") != 0) {
        return 1;
    }
    const char *text = spg_chat_history_text(&h);
    if (strstr(text, "AAA") != nullptr) {
        return 1; /* oldest evicted */
    }
    if (strstr(text, "DDD") == nullptr) {
        return 1; /* newest kept */
    }
    if (strncmp(text, "<start_of_turn>user", 19) != 0) {
        return 1; /* still begins at a user boundary */
    }
    return (h.used + 1u <= sizeof buf) ? 0 : 1;
}

static int test_oversize_turn(void) {
    char                    buf[24];
    struct spg_chat_history h;
    spg_chat_history_init(&h, sizeof buf, buf);

    /* A turn larger than the whole window cannot fit; transcript unchanged. */
    if (spg_chat_history_add_user(&h, "a message far larger than the buffer") !=
        SPG_E_LIMIT) {
        return 1;
    }
    return (h.used == 0u && spg_chat_history_text(&h)[0] == '\0') ? 0 : 1;
}

static int test_reset(void) {
    char                    buf[128];
    struct spg_chat_history h;
    spg_chat_history_init(&h, sizeof buf, buf);

    if (spg_chat_history_add_user(&h, "hi") != SPG_OK) {
        return 1;
    }
    spg_chat_history_reset(&h);
    return (h.used == 0u && spg_chat_history_text(&h)[0] == '\0') ? 0 : 1;
}

static int test_invalid_args(void) {
    char                    buf[64];
    struct spg_chat_history h;
    spg_chat_history_init(&h, sizeof buf, buf);

    if (spg_chat_history_add_user(nullptr, "x") != SPG_E_INVALID_ARG ||
        spg_chat_history_add_user(&h, nullptr) != SPG_E_INVALID_ARG ||
        spg_chat_history_add_assistant(&h, nullptr) != SPG_E_INVALID_ARG) {
        return 1;
    }
    return 0;
}

int main(void) {
    if (test_single_user_turn() != 0) {
        fprintf(stderr, "test_single_user_turn failed\n");
        return 1;
    }
    if (test_full_exchange() != 0) {
        fprintf(stderr, "test_full_exchange failed\n");
        return 1;
    }
    if (test_eviction() != 0) {
        fprintf(stderr, "test_eviction failed\n");
        return 1;
    }
    if (test_oversize_turn() != 0) {
        fprintf(stderr, "test_oversize_turn failed\n");
        return 1;
    }
    if (test_reset() != 0) {
        fprintf(stderr, "test_reset failed\n");
        return 1;
    }
    if (test_invalid_args() != 0) {
        fprintf(stderr, "test_invalid_args failed\n");
        return 1;
    }
    return 0;
}
