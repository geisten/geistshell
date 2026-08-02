#ifndef GEISTSHELL_MODEL_REMOTE_CODEC_H
#define GEISTSHELL_MODEL_REMOTE_CODEC_H

#include "geistshell/model_adapter.h"
#include "geistshell/status.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* OpenAI-compatible /v1/chat/completions request/response codec.
 *
 * These functions are pure: buffer in, buffer out, no network and no malloc in
 * our code. They are the testable core of the REMOTE model adapter and are
 * compiled (and unit-tested) in the default build, independent of libcurl. */

/* Build a chat/completions request body into `out`:
 *
 *   {"model":"<model_name>","messages":[{"role":"user","content":"<prompt>"}],
 *    "max_tokens":<N>,"temperature":<T>,"top_p":<P>,"stream":false}
 *
 * The prompt and model name are JSON-string-escaped. `out` is always
 * NUL-terminated. Returns SPG_E_LIMIT (and an empty `out`) if the body would
 * exceed out_cap — the codec is the single source of the size bound, so an
 * oversized prompt fails cleanly rather than overflowing. *out_used (optional)
 * receives the byte length written, excluding the NUL. */
[[nodiscard]] enum spg_status
spg_remote_build_request(const char *model_name, size_t prompt_n,
                         const char prompt[static prompt_n], size_t max_tokens,
                         float temperature, float top_p, size_t out_cap,
                         char out[static out_cap], size_t *out_used);

/* Parse a chat/completions response body. The text of
 * choices[0].message.content is JSON-unescaped into result->output, honouring
 * output_capacity (truncation sets output_truncated and the call returns
 * SPG_E_LIMIT). finish_reason maps to result flags ("stop" -> stopped_by_eos,
 * "length" -> stopped_by_token_limit); usage.completion_tokens maps to
 * tokens_decoded (defaulting to 1 when absent). All other result fields are
 * reset on entry.
 *
 * `tokens` is a caller-provided array of `token_cap` jsmntok_t entries, passed
 * as void* so this header need not include jsmn. A response with more JSON
 * tokens than `token_cap` yields SPG_E_LIMIT; malformed JSON yields
 * SPG_E_FORMAT; a well-formed body lacking a string content field yields
 * SPG_E_SCHEMA. */
[[nodiscard]] enum spg_status
spg_remote_parse_response(size_t body_n, const char body[static body_n],
                          size_t token_cap, void *tokens,
                          struct spg_model_generate_result *result);

#ifdef __cplusplus
}
#endif

#endif
