/* geistd_client.h — single-header C client for geistd, shaped like the
 * libgeist session API so an agent can switch between in-process libgeist
 * and a remote daemon without changing its loop.
 *
 *   #define GEISTD_CLIENT_IMPLEMENTATION   // in one translation unit
 *   #include "geistd_client.h"
 *
 *   struct geistd *g = geistd_connect_unix("/run/user/1000/geistd.sock", nullptr);
 *   char id[17]; geistd_open(g, 0.0f, 1.0f, 0, 0, id);
 *   geistd_prefill(g, id, n, ids, &prefilled, &reused);
 *   int32_t t; geistd_step(g, id, &t);
 *   float lp[2]; geistd_peek_logprobs(g, id, 2, cands, lp);
 *
 * Connects per call (geistd is serial; an idle connection blocks others).
 * Dependencies: jsmn.h next to this header (the same vendored copy).
 * Errors: every call returns 0 on success, -1 on failure with
 * geistd_error(g) set. Numbers in replies are parsed with strtod/strtol. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct geistd;

struct geistd *geistd_connect_unix(const char *path, const char *token);
struct geistd *geistd_connect_tcp(const char *host, int port, const char *token);
void           geistd_close(struct geistd *g);
const char    *geistd_error(const struct geistd *g);

int geistd_info(struct geistd *g, size_t cap, char json_out[static cap]); /* raw info JSON */
int geistd_open(struct geistd *g, float temperature, float top_p, int top_k, uint64_t seed, char id_out[static 17]);
int geistd_close_session(struct geistd *g, const char *id);
int geistd_reset(struct geistd *g, const char *id);
int geistd_tokenize(struct geistd *g, const char *text, size_t cap, int32_t out[static cap], size_t *n_out);
int geistd_prefill(struct geistd *g, const char *id, size_t n, const int32_t ids[static n], size_t *prefilled,
                   size_t *reused);
int geistd_step(struct geistd *g, const char *id, int32_t *token_out, bool *stop_out);
/* log-softmax logprobs of the pending distribution at the given ids. */
int geistd_peek_logprobs(struct geistd *g, const char *id, size_t n, const int32_t ids[static n], float out[static n]);
/* The whole pending logit vector (vocab floats) into `out`; *n_out = vocab. */
int geistd_peek_full(struct geistd *g, const char *id, size_t cap, float out[static cap], size_t *n_out);
/* Pieces for ids[0..n): each malloc'd (nullptr for control tokens); free them. Chunks internally. */
int geistd_strs(struct geistd *g, const char *id, size_t n, const int32_t ids[static n], char *out[static n]);
/* Pin the first n history tokens; reset keeps them afterwards. */
int geistd_pin(struct geistd *g, const char *id, size_t n);
/* A few info numbers without parsing JSON yourself. */
int geistd_info_numbers(struct geistd *g, size_t *vocab, int32_t *eos, int32_t *bos, bool *add_bos);
/* Streams pieces to `emit` (return false to stop). reason_out: "stop" | "max" | ... */
int geistd_generate(struct geistd *g, const char *id, size_t max, bool (*emit)(void *, const char *), void *ctx,
                    char reason_out[static 16]);

#ifdef GEISTD_CLIENT_IMPLEMENTATION
#define JSMN_STATIC
#define JSMN_STRICT
#define JSMN_PARENT_LINKS
#include "jsmn.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct geistd {
    char  path[256];
    char  host[128];
    int   port;
    char  token[128];
    char  err[256];
    int   fd;
    /* last reply */
    char *hdr;
    size_t hl;
    unsigned char *body;
    size_t bl;
    jsmntok_t tok[4096];
    int   ntok;
};

static struct geistd *gd_alloc(const char *token) {
    struct geistd *g = calloc(1, sizeof *g);
    if (g && token) snprintf(g->token, sizeof g->token, "%s", token);
    if (g) g->fd = -1;
    return g;
}

struct geistd *geistd_connect_unix(const char *path, const char *token) {
    struct geistd *g = gd_alloc(token);
    if (g) snprintf(g->path, sizeof g->path, "%s", path);
    return g;
}

struct geistd *geistd_connect_tcp(const char *host, int port, const char *token) {
    struct geistd *g = gd_alloc(token);
    if (g) {
        snprintf(g->host, sizeof g->host, "%s", host);
        g->port = port;
    }
    return g;
}

void geistd_close(struct geistd *g) {
    if (!g) return;
    if (g->fd >= 0) close(g->fd);
    free(g->hdr);
    free(g->body);
    free(g);
}

const char *geistd_error(const struct geistd *g) { return g ? g->err : "no client"; }

static int gd_fail(struct geistd *g, const char *msg) {
    snprintf(g->err, sizeof g->err, "%s", msg);
    if (g->fd >= 0) close(g->fd);
    g->fd = -1;
    return -1;
}

static bool gd_write(struct geistd *g, size_t n, const void *buf) {
    const unsigned char *p = buf;
    while (n) {
        ssize_t w = write(g->fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        p += w, n -= (size_t) w;
    }
    return true;
}

static bool gd_read(struct geistd *g, size_t n, void *buf) {
    unsigned char *p = buf;
    while (n) {
        ssize_t r = read(g->fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        p += r, n -= (size_t) r;
    }
    return true;
}

static bool gd_send(struct geistd *g, const char *hdr, size_t bl, const void *body) {
    unsigned char pre[8];
    uint32_t      hl = (uint32_t) strlen(hdr);
    for (int i = 0; i < 4; i++) pre[i] = (unsigned char) (hl >> (8 * i)), pre[4 + i] = (unsigned char) (bl >> (8 * i));
    return gd_write(g, 8, pre) && gd_write(g, hl, hdr) && (bl == 0 || gd_write(g, bl, body));
}

/* Reads one frame into g->hdr/body and parses the header. */
static int gd_recv(struct geistd *g) {
    unsigned char pre[8];
    if (!gd_read(g, 8, pre)) return gd_fail(g, "connection closed by geistd");
    size_t hl = 0, bl = 0;
    for (int i = 3; i >= 0; i--) hl = hl << 8 | pre[i], bl = bl << 8 | pre[4 + i];
    if (hl == 0 || hl > (64u << 10) || bl > (16u << 20)) return gd_fail(g, "bad frame from geistd");
    free(g->hdr), free(g->body);
    g->hdr  = malloc(hl + 1);
    g->body = bl ? malloc(bl) : nullptr;
    if (!g->hdr || (bl && !g->body)) return gd_fail(g, "out of memory");
    if (!gd_read(g, hl, g->hdr) || (bl && !gd_read(g, bl, g->body))) return gd_fail(g, "short frame from geistd");
    g->hdr[hl] = '\0', g->hl = hl, g->bl = bl;
    jsmn_parser p;
    jsmn_init(&p);
    g->ntok = jsmn_parse(&p, g->hdr, hl, g->tok, 4096);
    if (g->ntok < 1) return gd_fail(g, "unparsable header from geistd");
    return 0;
}

static int gd_find(const struct geistd *g, const char *key) {
    size_t kl = strlen(key);
    for (int i = 1; i < g->ntok; i++)
        if (g->tok[i].parent == 0 && g->tok[i].type == JSMN_STRING && (size_t) (g->tok[i].end - g->tok[i].start) == kl &&
            memcmp(g->hdr + g->tok[i].start, key, kl) == 0 && i + 1 < g->ntok)
            return i + 1;
    return -1;
}

static int gd_check_ok(struct geistd *g) {
    int t = gd_find(g, "ok");
    if (t >= 0 && g->hdr[g->tok[t].start] == 't') return 0;
    int e = gd_find(g, "error");
    if (e >= 0) snprintf(g->err, sizeof g->err, "%.*s", g->tok[e].end - g->tok[e].start, g->hdr + g->tok[e].start);
    else snprintf(g->err, sizeof g->err, "geistd: not ok");
    close(g->fd), g->fd = -1;
    return -1;
}

static int gd_connect(struct geistd *g) {
    if (g->host[0]) {
        struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *res;
        char            port[8];
        snprintf(port, sizeof port, "%d", g->port);
        if (getaddrinfo(g->host, port, &hints, &res) != 0) return gd_fail(g, "resolve failed");
        g->fd = socket(res->ai_family, res->ai_socktype, 0);
        if (g->fd < 0 || connect(g->fd, res->ai_addr, res->ai_addrlen) != 0) {
            freeaddrinfo(res);
            return gd_fail(g, "connect failed");
        }
        freeaddrinfo(res);
    } else {
        struct sockaddr_un sa = {.sun_family = AF_UNIX};
        snprintf(sa.sun_path, sizeof sa.sun_path, "%s", g->path);
        g->fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (g->fd < 0 || connect(g->fd, (struct sockaddr *) &sa, sizeof sa) != 0) return gd_fail(g, "connect failed");
    }
    if (g->token[0]) {
        char h[200];
        snprintf(h, sizeof h, "{\"op\":\"hello\",\"token\":\"%s\"}", g->token);
        if (!gd_send(g, h, 0, nullptr) || gd_recv(g) != 0 || gd_check_ok(g) != 0) return -1;
    }
    return 0;
}

/* One round trip: connect, send, receive, check ok. Leaves the reply in g. */
static int gd_call(struct geistd *g, const char *hdr, size_t bl, const void *body) {
    if (gd_connect(g) != 0) return -1;
    if (!gd_send(g, hdr, bl, body)) return gd_fail(g, "send failed");
    if (gd_recv(g) != 0) return -1;
    int rc = gd_check_ok(g);
    if (g->fd >= 0) close(g->fd), g->fd = -1;
    return rc;
}

int geistd_info(struct geistd *g, size_t cap, char out[static cap]) {
    if (gd_call(g, "{\"op\":\"info\"}", 0, nullptr) != 0) return -1;
    snprintf(out, cap, "%s", g->hdr);
    return 0;
}

int geistd_open(struct geistd *g, float temperature, float top_p, int top_k, uint64_t seed, char id_out[static 17]) {
    char h[200];
    snprintf(h, sizeof h, "{\"op\":\"open\",\"temperature\":%g,\"top_p\":%g,\"top_k\":%d,\"seed\":%llu}", (double) temperature,
             (double) top_p, top_k, (unsigned long long) seed);
    if (gd_call(g, h, 0, nullptr) != 0) return -1;
    int t = gd_find(g, "session");
    if (t < 0 || g->tok[t].end - g->tok[t].start != 16) return gd_fail(g, "no session id in reply");
    memcpy(id_out, g->hdr + g->tok[t].start, 16), id_out[16] = '\0';
    return 0;
}

static int gd_simple(struct geistd *g, const char *op, const char *id) {
    char h[100];
    snprintf(h, sizeof h, "{\"op\":\"%s\",\"session\":\"%s\"}", op, id);
    return gd_call(g, h, 0, nullptr);
}

int geistd_close_session(struct geistd *g, const char *id) { return gd_simple(g, "close", id); }
int geistd_reset(struct geistd *g, const char *id) { return gd_simple(g, "reset", id); }

int geistd_tokenize(struct geistd *g, const char *text, size_t cap, int32_t out[static cap], size_t *n_out) {
    size_t tl = strlen(text);
    char  *h  = malloc(tl * 6 + 32);
    if (!h) return gd_fail(g, "out of memory");
    char *p = h + snprintf(h, tl * 6 + 32, "{\"op\":\"tokenize\",\"text\":\"");
    for (const char *s = text; *s; s++) {
        unsigned char c = (unsigned char) *s;
        if (c == '"' || c == '\\') *p++ = '\\', *p++ = (char) c;
        else if (c == '\n') *p++ = '\\', *p++ = 'n';
        else if (c < 0x20) p += snprintf(p, 8, "\\u%04x", c);
        else *p++ = (char) c;
    }
    strcpy(p, "\"}");
    int rc = gd_call(g, h, 0, nullptr);
    free(h);
    if (rc != 0) return -1;
    size_t n = g->bl / sizeof(int32_t);
    if (n > cap) return gd_fail(g, "too many tokens for the buffer");
    memcpy(out, g->body, n * sizeof(int32_t));
    *n_out = n;
    return 0;
}

static size_t gd_num(const struct geistd *g, const char *key) {
    int t = gd_find(g, key);
    return t < 0 ? 0 : (size_t) strtoul(g->hdr + g->tok[t].start, nullptr, 10);
}

int geistd_prefill(struct geistd *g, const char *id, size_t n, const int32_t ids[static n], size_t *prefilled,
                   size_t *reused) {
    char h[100];
    snprintf(h, sizeof h, "{\"op\":\"prefill\",\"session\":\"%s\"}", id);
    if (gd_call(g, h, n * sizeof(int32_t), ids) != 0) return -1;
    if (prefilled) *prefilled = gd_num(g, "prefilled");
    if (reused) *reused = gd_num(g, "reused");
    return 0;
}

int geistd_step(struct geistd *g, const char *id, int32_t *token_out, bool *stop_out) {
    char h[100];
    snprintf(h, sizeof h, "{\"op\":\"step\",\"session\":\"%s\"}", id);
    if (gd_call(g, h, 0, nullptr) != 0) return -1;
    int t = gd_find(g, "token");
    if (t < 0) return gd_fail(g, "no token in reply");
    *token_out = (int32_t) strtol(g->hdr + g->tok[t].start, nullptr, 10);
    int s = gd_find(g, "stop");
    if (stop_out) *stop_out = s >= 0 && g->hdr[g->tok[s].start] == 't';
    return 0;
}

int geistd_peek_logprobs(struct geistd *g, const char *id, size_t n, const int32_t ids[static n], float out[static n]) {
    char h[100];
    snprintf(h, sizeof h, "{\"op\":\"peek\",\"session\":\"%s\"}", id);
    if (gd_call(g, h, n * sizeof(int32_t), ids) != 0) return -1;
    int arr = gd_find(g, "logprobs");
    if (arr < 0 || g->tok[arr].type != JSMN_ARRAY) return gd_fail(g, "no logprobs in reply");
    size_t k = 0;
    for (int i = arr + 1; i < g->ntok && k < n; i++)
        if (g->tok[i].parent == arr) out[k++] = (float) strtod(g->hdr + g->tok[i].start, nullptr);
    return k == n ? 0 : gd_fail(g, "short logprobs");
}

int geistd_peek_full(struct geistd *g, const char *id, size_t cap, float out[static cap], size_t *n_out) {
    char h[120];
    snprintf(h, sizeof h, "{\"op\":\"peek\",\"session\":\"%s\",\"full\":true}", id);
    if (gd_call(g, h, 0, nullptr) != 0) return -1;
    size_t n = g->bl / sizeof(float);
    if (n == 0 || n > cap) return gd_fail(g, "logit vector does not fit");
    memcpy(out, g->body, n * sizeof(float));
    *n_out = n;
    return 0;
}

/* JSON string token → malloc'd unescaped C string. */
static char *gd_unescape(const char *s, const char *e) {
    char  *out = malloc((size_t) (e - s) + 1);
    size_t o   = 0;
    if (!out) return nullptr;
    for (; s < e; s++) {
        if (*s != '\\' || s + 1 >= e) { out[o++] = *s; continue; }
        s++;
        switch (*s) {
        case 'n': out[o++] = '\n'; break;
        case 't': out[o++] = '\t'; break;
        case 'r': out[o++] = '\r'; break;
        case 'b': out[o++] = '\b'; break;
        case 'f': out[o++] = '\f'; break;
        case 'u': {
            unsigned cp = 0;
            if (s + 4 < e) { char hex[5] = {s[1], s[2], s[3], s[4], 0}; cp = (unsigned) strtoul(hex, nullptr, 16); s += 4; }
            if (cp < 0x80) out[o++] = (char) cp;
            else if (cp < 0x800) { out[o++] = (char) (0xC0 | cp >> 6); out[o++] = (char) (0x80 | (cp & 0x3F)); }
            else { out[o++] = (char) (0xE0 | cp >> 12); out[o++] = (char) (0x80 | ((cp >> 6) & 0x3F)); out[o++] = (char) (0x80 | (cp & 0x3F)); }
            break;
        }
        default: out[o++] = *s;
        }
    }
    out[o] = '\0';
    return out;
}

int geistd_strs(struct geistd *g, const char *id, size_t n, const int32_t ids[static n], char *out[static n]) {
    char h[100];
    snprintf(h, sizeof h, "{\"op\":\"str\",\"session\":\"%s\"}", id);
    for (size_t i = 0; i < n; i++) out[i] = nullptr;
    const size_t CHUNK = 2048; /* 2048 pieces stay under the 64 KiB header cap */
    for (size_t at = 0; at < n; at += CHUNK) {
        size_t m = n - at < CHUNK ? n - at : CHUNK;
        if (gd_call(g, h, m * sizeof(int32_t), ids + at) != 0) return -1;
        int arr = gd_find(g, "pieces");
        if (arr < 0 || g->tok[arr].type != JSMN_ARRAY) return gd_fail(g, "no pieces in reply");
        size_t k = 0;
        for (int i = arr + 1; i < g->ntok && k < m; i++) {
            if (g->tok[i].parent != arr) continue;
            out[at + k] = g->tok[i].type == JSMN_STRING ? gd_unescape(g->hdr + g->tok[i].start, g->hdr + g->tok[i].end) : nullptr;
            k++;
        }
        if (k != m) return gd_fail(g, "short pieces");
    }
    return 0;
}

int geistd_pin(struct geistd *g, const char *id, size_t n) {
    char h[120];
    snprintf(h, sizeof h, "{\"op\":\"pin\",\"session\":\"%s\",\"n\":%zu}", id, n);
    return gd_call(g, h, 0, nullptr);
}

int geistd_info_numbers(struct geistd *g, size_t *vocab, int32_t *eos, int32_t *bos, bool *add_bos) {
    if (gd_call(g, "{\"op\":\"info\"}", 0, nullptr) != 0) return -1;
    if (vocab) *vocab = gd_num(g, "vocab");
    int te = gd_find(g, "eos"), tb = gd_find(g, "bos"), ta = gd_find(g, "add_bos");
    if (eos) *eos = te < 0 ? -1 : (int32_t) strtol(g->hdr + g->tok[te].start, nullptr, 10);
    if (bos) *bos = tb < 0 ? -1 : (int32_t) strtol(g->hdr + g->tok[tb].start, nullptr, 10);
    if (add_bos) *add_bos = ta >= 0 && g->hdr[g->tok[ta].start] == 't';
    return 0;
}

int geistd_generate(struct geistd *g, const char *id, size_t max, bool (*emit)(void *, const char *), void *ctx,
                    char reason_out[static 16]) {
    char h[120];
    snprintf(h, sizeof h, "{\"op\":\"generate\",\"session\":\"%s\",\"max\":%zu}", id, max);
    if (gd_connect(g) != 0) return -1;
    if (!gd_send(g, h, 0, nullptr)) return gd_fail(g, "send failed");
    for (;;) {
        if (gd_recv(g) != 0) return -1;
        int ok = gd_find(g, "ok");
        if (ok < 0 || g->hdr[g->tok[ok].start] != 't') return gd_check_ok(g);
        int done = gd_find(g, "done");
        if (done >= 0 && g->hdr[g->tok[done].start] == 't') {
            int r = gd_find(g, "reason");
            snprintf(reason_out, 16, "%.*s", r < 0 ? 0 : g->tok[r].end - g->tok[r].start, r < 0 ? "" : g->hdr + g->tok[r].start);
            close(g->fd), g->fd = -1;
            return 0;
        }
        int p = gd_find(g, "piece");
        char piece[256] = "";
        if (p >= 0 && g->tok[p].type == JSMN_STRING) {
            /* unescape the common cases; a client wanting exact bytes uses str/ids */
            const char *s = g->hdr + g->tok[p].start, *e = g->hdr + g->tok[p].end;
            size_t      o = 0;
            for (; s < e && o + 1 < sizeof piece; s++) {
                if (*s == '\\' && s + 1 < e) {
                    s++;
                    piece[o++] = *s == 'n' ? '\n' : *s == 't' ? '\t' : *s == 'r' ? '\r' : *s;
                } else piece[o++] = *s;
            }
            piece[o] = '\0';
        }
        if (emit && !emit(ctx, piece)) {
            close(g->fd), g->fd = -1;
            snprintf(reason_out, 16, "client");
            return 0;
        }
    }
}
#endif /* GEISTD_CLIENT_IMPLEMENTATION */
