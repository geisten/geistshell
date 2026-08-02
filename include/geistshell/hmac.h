#ifndef GEISTSHELL_HMAC_H
#define GEISTSHELL_HMAC_H

#include "geistshell/hash.h"
#include "geistshell/status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Keyed message authentication over the project hash (BLAKE3), via the standard
 * HMAC construction H((K' XOR opad) || H((K' XOR ipad) || msg)). A black-box
 * wrapper over the public hash API — it touches no crypto internals. BLAKE3 also
 * has a native keyed mode; HMAC is used here because it needs no changes to the
 * hash core and is a universally recognized MAC. Output is SPG_HASH_BYTES (32).
 *
 * The tag authenticates msg to holders of key (symmetric: anyone with the key
 * can both produce and verify — tamper-evidence, not third-party non-repudiation). */
[[nodiscard]] enum spg_status spg_hmac(size_t key_n, const uint8_t key[],
                                       size_t msg_n, const uint8_t msg[],
                                       size_t out_n,
                                       uint8_t out[static out_n]);

/* Constant-time equality of two SPG_HASH_BYTES tags (no early exit). */
[[nodiscard]] bool spg_hmac_equal(const uint8_t a[static SPG_HASH_BYTES],
                                  const uint8_t b[static SPG_HASH_BYTES]);

#ifdef __cplusplus
}
#endif

#endif
