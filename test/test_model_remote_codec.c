/* Unit tests for the OpenAI-compatible request/response codec. These are pure
 * (no network, no libcurl) and run in the default build. */
#define JSMN_HEADER
#include "jsmn.h" /* jsmntok_t for the caller-provided token array */

#include "geistshell/model_remote_codec.h"

#include <stdio.h>
#include <string.h>

static jsmntok_t g_tokens[512];

/* ----- request building ------------------------------------------------- */

static int test_build_basic(void) {
    char   out[512];
    size_t used = 0u;
    if (spg_remote_build_request("gpt-x", 2u, "hi", 16u, 0.0f, 1.0f, sizeof out,
                                 out, &used) != SPG_OK) {
        return 1;
    }
    if (used != strlen(out)) {
        return 1;
    }
    return (strstr(out, "\"model\":\"gpt-x\"") && strstr(out, "\"content\":\"hi\"") &&
            strstr(out, "\"max_tokens\":16") && strstr(out, "\"stream\":false"))
               ? 0
               : 1;
}

static int test_build_escaping(void) {
    char        out[512];
    const char *p = "a\"b\\c\nd\te\x01"; /* quote, backslash, newline, tab, ctrl */
    if (spg_remote_build_request("m", strlen(p), p, 8u, 0.0f, 1.0f, sizeof out,
                                 out, nullptr) != SPG_OK) {
        return 1;
    }
    return (strstr(out, "a\\\"b") && strstr(out, "b\\\\c") &&
            strstr(out, "c\\nd") && strstr(out, "d\\te") &&
            strstr(out, "\\u0001"))
               ? 0
               : 1;
}

static int test_build_overflow(void) {
    char out[8];
    if (spg_remote_build_request("modelname", 4u, "data", 16u, 0.0f, 1.0f,
                                 sizeof out, out, nullptr) != SPG_E_LIMIT) {
        return 1;
    }
    return out[0] == '\0' ? 0 : 1;
}

static int test_build_invalid_args(void) {
    char out[16];
    return spg_remote_build_request(nullptr, 1u, "x", 1u, 0.0f, 1.0f, sizeof out,
                                    out, nullptr) == SPG_E_INVALID_ARG
               ? 0
               : 1;
}

/* ----- response parsing ------------------------------------------------- */

static int test_parse_happy(void) {
    const char *body =
        "{\"id\":\"x\",\"object\":\"chat.completion\","
        "\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\","
        "\"content\":\"(recommend (kind finish))\"},\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":7,"
        "\"total_tokens\":17}}";
    char                             buf[256];
    struct spg_model_generate_result r = {.output_capacity = sizeof buf,
                                          .output          = buf};
    if (spg_remote_parse_response(strlen(body), body, 512u, g_tokens, &r) !=
        SPG_OK) {
        return 1;
    }
    if (strcmp(buf, "(recommend (kind finish))") != 0) {
        return 1;
    }
    if (!r.stopped_by_eos || r.stopped_by_token_limit || r.output_truncated) {
        return 1;
    }
    return r.tokens_decoded == 7u ? 0 : 1;
}

static int test_parse_finish_length(void) {
    const char *body =
        "{\"choices\":[{\"message\":{\"content\":\"x\"},"
        "\"finish_reason\":\"length\"}]}";
    char                             buf[64];
    struct spg_model_generate_result r = {.output_capacity = sizeof buf,
                                          .output          = buf};
    if (spg_remote_parse_response(strlen(body), body, 512u, g_tokens, &r) !=
        SPG_OK) {
        return 1;
    }
    if (!r.stopped_by_token_limit || r.stopped_by_eos) {
        return 1;
    }
    /* usage absent -> tokens_decoded defaults to 1 */
    return r.tokens_decoded == 1u ? 0 : 1;
}

static int test_parse_unescape(void) {
    const char *body =
        "{\"choices\":[{\"message\":{\"content\":"
        "\"a\\nb \\\"q\\\" \\\\ \\u00e9\"}}]}";
    const char expected[] = "a\nb \"q\" \\ \xc3\xa9"; /* U+00E9 -> C3 A9 */
    char       buf[64];
    struct spg_model_generate_result r = {.output_capacity = sizeof buf,
                                          .output          = buf};
    if (spg_remote_parse_response(strlen(body), body, 512u, g_tokens, &r) !=
        SPG_OK) {
        return 1;
    }
    if (r.output_used != sizeof expected - 1u) {
        return 1;
    }
    return memcmp(buf, expected, sizeof expected) == 0 ? 0 : 1;
}

static int test_parse_truncation(void) {
    const char *body =
        "{\"choices\":[{\"message\":{\"content\":\"0123456789\"}}]}";
    char                             buf[5]; /* room for 4 chars + NUL */
    struct spg_model_generate_result r = {.output_capacity = sizeof buf,
                                          .output          = buf};
    if (spg_remote_parse_response(strlen(body), body, 512u, g_tokens, &r) !=
        SPG_E_LIMIT) {
        return 1;
    }
    if (!r.output_truncated) {
        return 1;
    }
    return strcmp(buf, "0123") == 0 ? 0 : 1;
}

static int test_parse_missing_content(void) {
    const char *body =
        "{\"choices\":[{\"message\":{\"role\":\"assistant\"}}]}";
    char                             buf[64];
    struct spg_model_generate_result r = {.output_capacity = sizeof buf,
                                          .output          = buf};
    return spg_remote_parse_response(strlen(body), body, 512u, g_tokens, &r) ==
                   SPG_E_SCHEMA
               ? 0
               : 1;
}

static int test_parse_content_null(void) {
    const char *body = "{\"choices\":[{\"message\":{\"content\":null}}]}";
    char                             buf[64];
    struct spg_model_generate_result r = {.output_capacity = sizeof buf,
                                          .output          = buf};
    return spg_remote_parse_response(strlen(body), body, 512u, g_tokens, &r) ==
                   SPG_E_SCHEMA
               ? 0
               : 1;
}

static int test_parse_garbage(void) {
    const char *body = "not json{";
    char                             buf[64];
    struct spg_model_generate_result r = {.output_capacity = sizeof buf,
                                          .output          = buf};
    return spg_remote_parse_response(strlen(body), body, 512u, g_tokens, &r) ==
                   SPG_E_FORMAT
               ? 0
               : 1;
}

static int test_parse_token_overflow(void) {
    const char *body =
        "{\"choices\":[{\"message\":{\"content\":\"hello\"},"
        "\"finish_reason\":\"stop\"}]}";
    char                             buf[64];
    struct spg_model_generate_result r = {.output_capacity = sizeof buf,
                                          .output          = buf};
    /* far fewer tokens than the body needs -> SPG_E_LIMIT, not a crash */
    return spg_remote_parse_response(strlen(body), body, 3u, g_tokens, &r) ==
                   SPG_E_LIMIT
               ? 0
               : 1;
}

static int test_parse_invalid_args(void) {
    const char *body = "{}";
    struct spg_model_generate_result r = {.output_capacity = 0u,
                                          .output          = nullptr};
    return spg_remote_parse_response(strlen(body), body, 512u, g_tokens, &r) ==
                   SPG_E_INVALID_ARG
               ? 0
               : 1;
}

int main(void) {
    struct {
        const char *name;
        int (*fn)(void);
    } cases[] = {
        {"build_basic", test_build_basic},
        {"build_escaping", test_build_escaping},
        {"build_overflow", test_build_overflow},
        {"build_invalid_args", test_build_invalid_args},
        {"parse_happy", test_parse_happy},
        {"parse_finish_length", test_parse_finish_length},
        {"parse_unescape", test_parse_unescape},
        {"parse_truncation", test_parse_truncation},
        {"parse_missing_content", test_parse_missing_content},
        {"parse_content_null", test_parse_content_null},
        {"parse_garbage", test_parse_garbage},
        {"parse_token_overflow", test_parse_token_overflow},
        {"parse_invalid_args", test_parse_invalid_args},
    };
    for (size_t i = 0u; i < sizeof cases / sizeof cases[0]; i += 1u) {
        if (cases[i].fn() != 0) {
            fprintf(stderr, "test_model_remote_codec: %s failed\n",
                    cases[i].name);
            return 1;
        }
    }
    return 0;
}
