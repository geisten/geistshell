#include "geistshell/model_remote_codec.h"

#include <stdio.h>
#include <string.h>

/* jsmn is vendored third-party code and is not written to our warning bar;
 * isolate it behind diagnostic pragmas. JSMN_STATIC keeps every symbol
 * file-local (this is the one TU that emits the implementation); JSMN_STRICT
 * makes the tokenizer reject malformed JSON. */
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#define JSMN_STATIC
#define JSMN_STRICT
#include "jsmn.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

/* ----------------------------------------------------------------------------
 * Request building
 * ------------------------------------------------------------------------- */

/* Bounded forward-only writer over a caller buffer; reserves one byte for the
 * terminating NUL. Once an append would overflow, `ok` latches false and all
 * further appends are no-ops. */
struct wbuf {
    char  *p;
    size_t cap;
    size_t used;
    bool   ok;
};

static void w_raw(struct wbuf *w, const char *s, size_t n) {
    if (!w->ok) {
        return;
    }
    if (n + 1u > w->cap - w->used) {
        w->ok = false;
        return;
    }
    memcpy(w->p + w->used, s, n);
    w->used += n;
}

static void w_cstr(struct wbuf *w, const char *s) { w_raw(w, s, strlen(s)); }

/* Append `s` JSON-string-escaped (without surrounding quotes). */
static void w_escaped(struct wbuf *w, const char *s, size_t n) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0u; i < n && w->ok; i += 1u) {
        const unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '\"':
            w_raw(w, "\\\"", 2u);
            break;
        case '\\':
            w_raw(w, "\\\\", 2u);
            break;
        case '\b':
            w_raw(w, "\\b", 2u);
            break;
        case '\f':
            w_raw(w, "\\f", 2u);
            break;
        case '\n':
            w_raw(w, "\\n", 2u);
            break;
        case '\r':
            w_raw(w, "\\r", 2u);
            break;
        case '\t':
            w_raw(w, "\\t", 2u);
            break;
        default:
            if (c < 0x20u) {
                const char esc[6] = {'\\', 'u', '0', '0',
                                     hex[(c >> 4) & 0x0Fu], hex[c & 0x0Fu]};
                w_raw(w, esc, 6u);
            } else {
                const char ch = (char)c;
                w_raw(w, &ch, 1u);
            }
        }
    }
}

enum spg_status spg_remote_build_request(const char *model_name,
                                         const size_t prompt_n,
                                         const char prompt[static prompt_n],
                                         const size_t max_tokens,
                                         const float temperature,
                                         const float top_p, const size_t out_cap,
                                         char out[static out_cap],
                                         size_t *out_used) {
    if (model_name == nullptr || out == nullptr || out_cap == 0u ||
        (prompt_n > 0u && prompt == nullptr)) {
        return SPG_E_INVALID_ARG;
    }

    struct wbuf w = {.p = out, .cap = out_cap, .used = 0u, .ok = true};
    char        num[64];

    w_cstr(&w, "{\"model\":\"");
    w_escaped(&w, model_name, strlen(model_name));
    w_cstr(&w, "\",\"messages\":[{\"role\":\"user\",\"content\":\"");
    w_escaped(&w, prompt, prompt_n);
    w_cstr(&w, "\"}],\"max_tokens\":");
    (void)snprintf(num, sizeof num, "%zu", max_tokens);
    w_cstr(&w, num);
    w_cstr(&w, ",\"temperature\":");
    (void)snprintf(num, sizeof num, "%.3f", (double)temperature);
    w_cstr(&w, num);
    w_cstr(&w, ",\"top_p\":");
    (void)snprintf(num, sizeof num, "%.3f", (double)top_p);
    w_cstr(&w, num);
    w_cstr(&w, ",\"stream\":false}");

    if (!w.ok) {
        out[0] = '\0';
        if (out_used != nullptr) {
            *out_used = 0u;
        }
        return SPG_E_LIMIT;
    }
    out[w.used] = '\0';
    if (out_used != nullptr) {
        *out_used = w.used;
    }
    return SPG_OK;
}

/* ----------------------------------------------------------------------------
 * Response parsing
 * ------------------------------------------------------------------------- */

/* Returns the token index one past the subtree rooted at `i` (skips an object's
 * keys+values and an array's elements recursively). */
static int tok_end(const jsmntok_t *t, const int count, const int i) {
    if (i < 0 || i >= count) {
        return count;
    }
    int j = i + 1;
    if (t[i].type == JSMN_OBJECT) {
        for (int m = 0; m < t[i].size; m += 1) {
            j = tok_end(t, count, j); /* key   */
            j = tok_end(t, count, j); /* value */
        }
    } else if (t[i].type == JSMN_ARRAY) {
        for (int m = 0; m < t[i].size; m += 1) {
            j = tok_end(t, count, j);
        }
    }
    return j;
}

/* True when token `tok` is a string whose (unescaped-assumed) text equals key. */
static bool tok_str_eq(const char *js, const jsmntok_t *tok, const char *key) {
    if (tok->type != JSMN_STRING) {
        return false;
    }
    const size_t n = (size_t)(tok->end - tok->start);
    return strlen(key) == n && strncmp(js + tok->start, key, n) == 0;
}

/* Returns the value-token index for `key` in the object at `obj`, or -1. */
static int obj_get(const jsmntok_t *t, const int count, const int obj,
                   const char *js, const char *key) {
    if (obj < 0 || obj >= count || t[obj].type != JSMN_OBJECT) {
        return -1;
    }
    int j = obj + 1;
    for (int m = 0; m < t[obj].size; m += 1) {
        const int keytok = j;
        const int valtok = keytok + 1; /* key is a scalar string */
        if (valtok >= count) {
            return -1;
        }
        if (tok_str_eq(js, &t[keytok], key)) {
            return valtok;
        }
        j = tok_end(t, count, valtok);
    }
    return -1;
}

/* Bounded appender into result->output, mirroring the adapter's append-bytes
 * semantics: truncation latches `overflow`, sets output_truncated, and the
 * caller returns SPG_E_LIMIT. */
struct oacc {
    struct spg_model_generate_result *r;
    bool                              overflow;
};

static void o_put(struct oacc *o, const char *bytes, size_t n) {
    struct spg_model_generate_result *r = o->r;
    if (o->overflow || n == 0u) {
        return;
    }
    if (n + 1u > r->output_capacity - r->output_used) {
        const size_t avail = (r->output_capacity > r->output_used + 1u)
                                 ? r->output_capacity - r->output_used - 1u
                                 : 0u;
        if (avail > 0u) {
            memcpy(r->output + r->output_used, bytes, avail);
            r->output_used += avail;
        }
        r->output[r->output_used] = '\0';
        r->output_truncated       = true;
        o->overflow               = true;
        return;
    }
    memcpy(r->output + r->output_used, bytes, n);
    r->output_used += n;
    r->output[r->output_used] = '\0';
}

static void utf8_emit(struct oacc *o, const unsigned cp) {
    char b[4];
    if (cp < 0x80u) {
        b[0] = (char)cp;
        o_put(o, b, 1u);
    } else if (cp < 0x800u) {
        b[0] = (char)(0xC0u | (cp >> 6));
        b[1] = (char)(0x80u | (cp & 0x3Fu));
        o_put(o, b, 2u);
    } else if (cp < 0x10000u) {
        b[0] = (char)(0xE0u | (cp >> 12));
        b[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        b[2] = (char)(0x80u | (cp & 0x3Fu));
        o_put(o, b, 3u);
    } else {
        b[0] = (char)(0xF0u | (cp >> 18));
        b[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        b[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        b[3] = (char)(0x80u | (cp & 0x3Fu));
        o_put(o, b, 4u);
    }
}

static unsigned hex4(const char *s) {
    unsigned v = 0u;
    for (int i = 0; i < 4; i += 1) {
        const char c = s[i];
        unsigned   d = 0u;
        if (c >= '0' && c <= '9') {
            d = (unsigned)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = (unsigned)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            d = (unsigned)(c - 'A' + 10);
        }
        v = (v << 4) | d;
    }
    return v;
}

/* JSON-unescape `s` (length n) into the output accumulator. jsmn (strict) has
 * already validated escape syntax, so \uXXXX always has four hex digits. */
static void unescape_into(struct oacc *o, const char *s, const size_t n) {
    size_t i = 0u;
    while (i < n) {
        const char c = s[i];
        if (c != '\\') {
            o_put(o, &s[i], 1u);
            i += 1u;
            continue;
        }
        if (i + 1u >= n) {
            break;
        }
        const char e = s[i + 1u];
        switch (e) {
        case '\"':
            o_put(o, "\"", 1u);
            i += 2u;
            break;
        case '\\':
            o_put(o, "\\", 1u);
            i += 2u;
            break;
        case '/':
            o_put(o, "/", 1u);
            i += 2u;
            break;
        case 'b':
            o_put(o, "\b", 1u);
            i += 2u;
            break;
        case 'f':
            o_put(o, "\f", 1u);
            i += 2u;
            break;
        case 'n':
            o_put(o, "\n", 1u);
            i += 2u;
            break;
        case 'r':
            o_put(o, "\r", 1u);
            i += 2u;
            break;
        case 't':
            o_put(o, "\t", 1u);
            i += 2u;
            break;
        case 'u':
            if (i + 6u <= n) {
                unsigned cp = hex4(&s[i + 2u]);
                i += 6u;
                /* Combine a UTF-16 surrogate pair into one code point. */
                if (cp >= 0xD800u && cp <= 0xDBFFu && i + 6u <= n &&
                    s[i] == '\\' && s[i + 1u] == 'u') {
                    const unsigned lo = hex4(&s[i + 2u]);
                    if (lo >= 0xDC00u && lo <= 0xDFFFu) {
                        cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                        i += 6u;
                    }
                }
                utf8_emit(o, cp);
            } else {
                i += 2u;
            }
            break;
        default:
            o_put(o, &e, 1u);
            i += 2u;
            break;
        }
    }
}

enum spg_status spg_remote_parse_response(
    const size_t body_n, const char body[static body_n], const size_t token_cap,
    void *tokens, struct spg_model_generate_result *result) {
    if (body == nullptr || tokens == nullptr || result == nullptr ||
        result->output == nullptr || result->output_capacity == 0u ||
        token_cap == 0u) {
        return SPG_E_INVALID_ARG;
    }

    result->output_used            = 0u;
    result->output[0]              = '\0';
    result->output_truncated       = false;
    result->tokens_decoded         = 0u;
    result->stopped_by_eos         = false;
    result->stopped_by_token_limit = false;

    jsmntok_t  *tok = (jsmntok_t *)tokens;
    jsmn_parser parser;
    jsmn_init(&parser);
    const int r = jsmn_parse(&parser, body, body_n, tok, (unsigned)token_cap);
    if (r == JSMN_ERROR_NOMEM) {
        return SPG_E_LIMIT;
    }
    if (r < 1 || tok[0].type != JSMN_OBJECT) {
        return SPG_E_FORMAT;
    }
    const int count = r;

    const int choices = obj_get(tok, count, 0, body, "choices");
    if (choices < 0 || tok[choices].type != JSMN_ARRAY ||
        tok[choices].size < 1) {
        return SPG_E_SCHEMA;
    }
    const int choice0 = choices + 1; /* first array element */
    const int message = obj_get(tok, count, choice0, body, "message");
    if (message < 0) {
        return SPG_E_SCHEMA;
    }
    const int content = obj_get(tok, count, message, body, "content");
    if (content < 0 || tok[content].type != JSMN_STRING) {
        return SPG_E_SCHEMA;
    }

    struct oacc o = {.r = result, .overflow = false};
    unescape_into(&o, body + tok[content].start,
                  (size_t)(tok[content].end - tok[content].start));

    const int fr = obj_get(tok, count, choice0, body, "finish_reason");
    if (fr >= 0 && tok[fr].type == JSMN_STRING) {
        if (tok_str_eq(body, &tok[fr], "stop")) {
            result->stopped_by_eos = true;
        } else if (tok_str_eq(body, &tok[fr], "length")) {
            result->stopped_by_token_limit = true;
        }
    }

    const int usage = obj_get(tok, count, 0, body, "usage");
    if (usage >= 0) {
        const int ct = obj_get(tok, count, usage, body, "completion_tokens");
        if (ct >= 0 && tok[ct].type == JSMN_PRIMITIVE) {
            size_t v = 0u;
            for (int k = tok[ct].start; k < tok[ct].end; k += 1) {
                const char d = body[k];
                if (d < '0' || d > '9') {
                    break;
                }
                v = v * 10u + (size_t)(d - '0');
            }
            result->tokens_decoded = v;
        }
    }
    if (result->tokens_decoded == 0u) {
        result->tokens_decoded = 1u;
    }

    return o.overflow ? SPG_E_LIMIT : SPG_OK;
}
