#ifndef GEISTSHELL_CHAT_TEMPLATE_H
#define GEISTSHELL_CHAT_TEMPLATE_H

#include "geistshell/status.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Accumulates a Gemma multi-turn chat transcript in a caller-provided buffer,
 * ready to feed to the model each turn. The buffer always begins at a
 * "<start_of_turn>user" boundary; when a new turn would overflow, the oldest
 * whole exchange is evicted from the front so the most recent context is kept.
 * Call add_user and add_assistant in alternation. */
struct spg_chat_history {
    char  *buffer;
    size_t capacity; /* bytes of buffer, including the NUL terminator */
    size_t used;     /* transcript length, excluding the terminator */
};

/* Bind history to buffer (capacity > 0) and reset it to empty. */
void spg_chat_history_init(struct spg_chat_history *history, size_t capacity,
                           char buffer[]);

/* Clear the transcript (start a fresh conversation). */
void spg_chat_history_reset(struct spg_chat_history *history);

/* Append a user turn:
 *   "<start_of_turn>user\n{message}<end_of_turn>\n<start_of_turn>model\n"
 * Evicts the oldest exchanges from the front to make room. Returns
 * SPG_E_INVALID_ARG on null arguments, SPG_E_LIMIT if the turn cannot fit even
 * after evicting everything (transcript left unchanged in that case). */
[[nodiscard]] enum spg_status
spg_chat_history_add_user(struct spg_chat_history *history, const char *message);

/* Close the open model turn with the reply text:
 *   "{reply}<end_of_turn>\n"
 * Must follow an add_user. Evicts older exchanges if needed. Returns
 * SPG_E_INVALID_ARG on null arguments, SPG_E_LIMIT if the reply cannot fit. */
[[nodiscard]] enum spg_status
spg_chat_history_add_assistant(struct spg_chat_history *history,
                               const char *reply);

/* The accumulated transcript (NUL-terminated); "" when empty/unbound. */
[[nodiscard]] const char *
spg_chat_history_text(const struct spg_chat_history *history);

#ifdef __cplusplus
}
#endif

#endif
