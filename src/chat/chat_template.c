#include "geistshell/chat_template.h"

#include <string.h>

#define USER_OPEN  "<start_of_turn>user\n"
#define TURN_CLOSE "<end_of_turn>\n"
#define MODEL_OPEN "<start_of_turn>model\n"
#define USER_MARK  "<start_of_turn>user"

void spg_chat_history_init(struct spg_chat_history *history,
                           const size_t capacity, char buffer[]) {
    if (history == nullptr) {
        return;
    }
    history->buffer   = buffer;
    history->capacity = capacity;
    history->used     = 0u;
    if (buffer != nullptr && capacity > 0u) {
        buffer[0] = '\0';
    }
}

void spg_chat_history_reset(struct spg_chat_history *history) {
    if (history == nullptr) {
        return;
    }
    history->used = 0u;
    if (history->buffer != nullptr && history->capacity > 0u) {
        history->buffer[0] = '\0';
    }
}

/* Drop the oldest exchange by sliding the buffer to the next user turn.
 * Returns false when there is nothing earlier to drop. */
static bool evict_oldest(struct spg_chat_history *history) {
    if (history->used == 0u) {
        return false;
    }
    const char *next = strstr(history->buffer + 1, USER_MARK);
    if (next == nullptr) {
        return false; /* only one (or zero) exchanges remain */
    }
    const size_t drop = (size_t)(next - history->buffer);
    memmove(history->buffer, next, history->used - drop + 1u); /* incl. NUL */
    history->used -= drop;
    return true;
}

static void raw_append(struct spg_chat_history *history, const char *text) {
    const size_t n = strlen(text);
    memcpy(history->buffer + history->used, text, n);
    history->used += n;
    history->buffer[history->used] = '\0';
}

/* Make room for need bytes (plus the terminator), evicting oldest exchanges.
 * Returns true once it fits. */
static bool make_room(struct spg_chat_history *history, const size_t need) {
    while (history->used + need + 1u > history->capacity) {
        if (!evict_oldest(history)) {
            return false;
        }
    }
    return true;
}

enum spg_status spg_chat_history_add_user(struct spg_chat_history *history,
                                          const char              *message) {
    if (history == nullptr || history->buffer == nullptr ||
        message == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    const size_t need = strlen(USER_OPEN) + strlen(message) +
                        strlen(TURN_CLOSE) + strlen(MODEL_OPEN);
    if (!make_room(history, need)) {
        return SPG_E_LIMIT;
    }
    raw_append(history, USER_OPEN);
    raw_append(history, message);
    raw_append(history, TURN_CLOSE);
    raw_append(history, MODEL_OPEN);
    return SPG_OK;
}

enum spg_status spg_chat_history_add_assistant(struct spg_chat_history *history,
                                               const char              *reply) {
    if (history == nullptr || history->buffer == nullptr || reply == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    const size_t need = strlen(reply) + strlen(TURN_CLOSE);
    if (!make_room(history, need)) {
        return SPG_E_LIMIT;
    }
    raw_append(history, reply);
    raw_append(history, TURN_CLOSE);
    return SPG_OK;
}

const char *spg_chat_history_text(const struct spg_chat_history *history) {
    if (history == nullptr || history->buffer == nullptr) {
        return "";
    }
    return history->buffer;
}
