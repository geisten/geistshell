#include "geistshell/model_remote.h"

#ifdef SPG_ENABLE_REMOTE

#include "geistshell/model_remote_codec.h"

#include <curl/curl.h>
#include <stdio.h>
#include <string.h>

/* jsmntok_t for the response parser's caller-provided token array. */
#define JSMN_HEADER
#include "jsmn.h"

/* Bounds for the file-scope scratch buffers below. The runtime is
 * single-threaded and runs one model adapter, so static scratch is safe here
 * (the same assumption journal_sign.c already makes). Request: a prompt up to
 * the CLI's ~32 KB context plus JSON-escape expansion -> a 96 KB floor.
 * Response: model output (~8 KB) plus the JSON envelope/usage. */
#define SPG_REMOTE_REQ_BYTES  (96u * 1024u)
#define SPG_REMOTE_RESP_BYTES (64u * 1024u)
#define SPG_REMOTE_MAX_TOKENS 4096u
#define SPG_REMOTE_AUTH_BYTES 1024u
#define SPG_REMOTE_TIMEOUT_S  60L
#define SPG_REMOTE_CONNECT_S  10L

static char      g_req[SPG_REMOTE_REQ_BYTES];
static char      g_resp[SPG_REMOTE_RESP_BYTES];
static jsmntok_t g_tokens[SPG_REMOTE_MAX_TOKENS];

/* Bounded sink for the response body. Overflow (a response larger than the
 * scratch) returns 0 from the callback, which makes libcurl abort the transfer
 * with CURLE_WRITE_ERROR rather than silently truncating the JSON. */
struct resp_sink {
    char  *buf;
    size_t cap;
    size_t used;
    bool   overflow;
};

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    struct resp_sink *s = (struct resp_sink *)userdata;
    const size_t      n = size * nmemb;
    if (s->overflow || n > s->cap - s->used) {
        s->overflow = true;
        return 0u;
    }
    memcpy(s->buf + s->used, ptr, n);
    s->used += n;
    return n;
}

enum spg_status spg_remote_init(struct spg_model_adapter *adapter,
                                const struct spg_model_adapter_config *config) {
    if (config->endpoint_url == nullptr || config->endpoint_url[0] == '\0' ||
        config->model_name == nullptr || config->model_name[0] == '\0') {
        return SPG_E_INVALID_ARG;
    }
    /* Process-global; refcounted by libcurl and paired with cleanup in
     * spg_remote_destroy. Not thread-safe — single-threaded contract. */
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        return SPG_E_IO;
    }
    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        curl_global_cleanup();
        return SPG_E_IO;
    }

    adapter->http         = curl;
    adapter->endpoint_url = config->endpoint_url;
    adapter->model_name   = config->model_name;
    adapter->api_key      = config->api_key; /* may be null for local gateways */
    adapter->temperature  = config->sampling.temperature;
    adapter->top_p        = config->sampling.top_p;
    adapter->initialized  = true;
    return SPG_OK;
}

enum spg_status
spg_remote_generate(struct spg_model_adapter *adapter,
                    const struct spg_model_generate_request *request,
                    struct spg_model_generate_result *result) {
    CURL *curl = (CURL *)adapter->http;
    if (curl == nullptr) {
        return SPG_E_INVALID_STATE;
    }

    /* The codec owns the size bound: an oversized prompt fails cleanly here. */
    size_t                req_n = 0u;
    const enum spg_status bs    = spg_remote_build_request(
        adapter->model_name, request->prompt_n, request->prompt,
        request->max_decode_tokens, adapter->temperature, adapter->top_p,
        sizeof g_req, g_req, &req_n);
    if (bs != SPG_OK) {
        return bs;
    }

    struct curl_slist *headers =
        curl_slist_append(nullptr, "Content-Type: application/json");
    if (headers == nullptr) {
        return SPG_E_IO;
    }
    char auth[SPG_REMOTE_AUTH_BYTES];
    if (adapter->api_key != nullptr && adapter->api_key[0] != '\0') {
        const int an = snprintf(auth, sizeof auth, "Authorization: Bearer %s",
                                adapter->api_key);
        if (an > 0 && (size_t)an < sizeof auth) {
            struct curl_slist *with_auth = curl_slist_append(headers, auth);
            if (with_auth == nullptr) {
                curl_slist_free_all(headers);
                return SPG_E_IO;
            }
            headers = with_auth;
        }
    }

    struct resp_sink sink = {
        .buf = g_resp, .cap = sizeof g_resp, .used = 0u, .overflow = false};

    /* reset keeps the live connection/DNS caches (keep-alive across steps). */
    curl_easy_reset(curl);
    (void)curl_easy_setopt(curl, CURLOPT_URL, adapter->endpoint_url);
    (void)curl_easy_setopt(curl, CURLOPT_POST, 1L);
    (void)curl_easy_setopt(curl, CURLOPT_POSTFIELDS, g_req);
    (void)curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)req_n);
    (void)curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    (void)curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    (void)curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    (void)curl_easy_setopt(curl, CURLOPT_TIMEOUT, SPG_REMOTE_TIMEOUT_S);
    (void)curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, SPG_REMOTE_CONNECT_S);
    (void)curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    const CURLcode rc        = curl_easy_perform(curl);
    long           http_code = 0;
    (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    /* Best-effort: don't leave the bearer token resident on the stack. */
    memset(auth, 0, sizeof auth);

    if (rc != CURLE_OK) {
        return sink.overflow ? SPG_E_LIMIT : SPG_E_IO;
    }
    if (http_code < 200 || http_code >= 300) {
        return SPG_E_MODEL;
    }
    return spg_remote_parse_response(sink.used, g_resp, SPG_REMOTE_MAX_TOKENS,
                                     g_tokens, result);
}

void spg_remote_destroy(struct spg_model_adapter *adapter) {
    if (adapter->http != nullptr) {
        curl_easy_cleanup((CURL *)adapter->http);
        adapter->http = nullptr;
        curl_global_cleanup();
    }
}

#endif /* SPG_ENABLE_REMOTE */
