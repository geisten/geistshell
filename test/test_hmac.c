#include "geistshell/hmac.h"

#include <stdio.h>
#include <string.h>

static int tag(const char *key, const char *msg, uint8_t out[static 32]) {
    return spg_hmac(strlen(key), (const uint8_t *)key, strlen(msg),
                    (const uint8_t *)msg, 32u, out) == SPG_OK
               ? 0
               : 1;
}

/* Deterministic: same key + message -> same tag. */
static int test_deterministic(void) {
    uint8_t a[32];
    uint8_t b[32];
    if (tag("k", "hello", a) != 0 || tag("k", "hello", b) != 0) {
        return 1;
    }
    return spg_hmac_equal(a, b) ? 0 : 1;
}

/* A different key yields a different tag (key actually binds). */
static int test_key_sensitive(void) {
    uint8_t a[32];
    uint8_t b[32];
    if (tag("key-one", "msg", a) != 0 || tag("key-two", "msg", b) != 0) {
        return 1;
    }
    return spg_hmac_equal(a, b) ? 1 : 0;
}

/* A one-byte message change yields a different tag (forgery is detectable). */
static int test_message_sensitive(void) {
    uint8_t a[32];
    uint8_t b[32];
    if (tag("k", "report v1", a) != 0 || tag("k", "report v2", b) != 0) {
        return 1;
    }
    return spg_hmac_equal(a, b) ? 1 : 0;
}

/* Keys longer than the block size are accepted (hashed down). */
static int test_long_key(void) {
    char    longkey[200];
    memset(longkey, 'x', sizeof longkey - 1u);
    longkey[sizeof longkey - 1u] = '\0';
    uint8_t a[32];
    uint8_t b[32];
    if (tag(longkey, "m", a) != 0 || tag(longkey, "m", b) != 0) {
        return 1;
    }
    return spg_hmac_equal(a, b) ? 0 : 1;
}

static int test_invalid_args(void) {
    uint8_t out[32];
    /* wrong output length is rejected */
    if (spg_hmac(1u, (const uint8_t *)"k", 1u, (const uint8_t *)"m", 16u,
                 out) != SPG_E_INVALID_ARG) {
        return 1;
    }
    return 0;
}

int main(void) {
    if (test_deterministic() != 0) {
        fprintf(stderr, "test_deterministic failed\n");
        return 1;
    }
    if (test_key_sensitive() != 0) {
        fprintf(stderr, "test_key_sensitive failed\n");
        return 1;
    }
    if (test_message_sensitive() != 0) {
        fprintf(stderr, "test_message_sensitive failed\n");
        return 1;
    }
    if (test_long_key() != 0) {
        fprintf(stderr, "test_long_key failed\n");
        return 1;
    }
    if (test_invalid_args() != 0) {
        fprintf(stderr, "test_invalid_args failed\n");
        return 1;
    }
    return 0;
}
