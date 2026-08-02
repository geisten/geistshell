#include "geistshell/hmac.h"
#include "geistshell/journal.h"

#include <stdio.h>
#include <string.h>

#define SEAL_PAYLOAD_SCRATCH 8192u

/* Read the whole journal, verifying its hash chain, and return the final chain
 * hash (the last record's hash; zero for an empty journal). */
static enum spg_status chain_head(const char *path,
                                  uint8_t head[static SPG_JOURNAL_HASH_BYTES]) {
    struct spg_journal_reader reader = {};
    enum spg_status status = spg_journal_reader_open(&reader, path);
    if (status != SPG_OK) {
        return status;
    }
    memset(head, 0, SPG_JOURNAL_HASH_BYTES);
    static uint8_t            payload[SEAL_PAYLOAD_SCRATCH];
    struct spg_journal_record rec = {};
    for (;;) {
        status = spg_journal_reader_next(&reader, sizeof payload, payload, &rec);
        if (status == SPG_E_NOT_FOUND) {
            status = SPG_OK;
            break;
        }
        /* A record verified but larger than the scratch is fine — the chain and
         * last_hash are already updated; only the payload copy was truncated. */
        if (status != SPG_OK && status != SPG_E_LIMIT) {
            break;
        }
    }
    if (status == SPG_OK) {
        memcpy(head, reader.last_hash, SPG_JOURNAL_HASH_BYTES);
    }
    (void)spg_journal_reader_close(&reader);
    return status;
}

enum spg_status spg_journal_seal(const char *journal_path, const char *sig_path,
                                 const size_t key_n, const uint8_t key[]) {
    if (journal_path == nullptr || sig_path == nullptr ||
        (key_n > 0u && key == nullptr)) {
        return SPG_E_INVALID_ARG;
    }
    uint8_t               head[SPG_JOURNAL_HASH_BYTES];
    const enum spg_status s = chain_head(journal_path, head);
    if (s != SPG_OK) {
        return s;
    }
    uint8_t               tag[SPG_HASH_BYTES];
    const enum spg_status hs =
        spg_hmac(key_n, key, SPG_JOURNAL_HASH_BYTES, head, SPG_HASH_BYTES, tag);
    if (hs != SPG_OK) {
        return hs;
    }
    FILE *f = fopen(sig_path, "wb");
    if (f == nullptr) {
        return SPG_E_IO;
    }
    for (size_t i = 0u; i < SPG_HASH_BYTES; i += 1u) {
        if (fprintf(f, "%02x", tag[i]) != 2) {
            (void)fclose(f);
            return SPG_E_IO;
        }
    }
    (void)fputc('\n', f);
    return fclose(f) == 0 ? SPG_OK : SPG_E_IO;
}

static bool hex_nibble(const int c, uint8_t *out) {
    if (c >= '0' && c <= '9') {
        *out = (uint8_t)(c - '0');
    } else if (c >= 'a' && c <= 'f') {
        *out = (uint8_t)(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
        *out = (uint8_t)(c - 'A' + 10);
    } else {
        return false;
    }
    return true;
}

/* Read 2*SPG_HASH_BYTES hex chars from path into out. False on short/garbled. */
static bool read_hex_tag(const char *path,
                         uint8_t out[static SPG_HASH_BYTES]) {
    FILE *f = fopen(path, "rb");
    if (f == nullptr) {
        return false;
    }
    bool ok = true;
    for (size_t i = 0u; i < SPG_HASH_BYTES; i += 1u) {
        uint8_t hi = 0u;
        uint8_t lo = 0u;
        if (!hex_nibble(fgetc(f), &hi) || !hex_nibble(fgetc(f), &lo)) {
            ok = false;
            break;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    (void)fclose(f);
    return ok;
}

enum spg_status spg_journal_verify_signed(const char *journal_path,
                                          const char *sig_path,
                                          const size_t key_n,
                                          const uint8_t key[], bool *ok) {
    if (journal_path == nullptr || sig_path == nullptr || ok == nullptr ||
        (key_n > 0u && key == nullptr)) {
        return SPG_E_INVALID_ARG;
    }
    *ok = false;

    uint8_t               head[SPG_JOURNAL_HASH_BYTES];
    const enum spg_status s = chain_head(journal_path, head);
    if (s == SPG_E_JOURNAL_CORRUPT) {
        return SPG_OK; /* broken chain -> not verified, but the check ran */
    }
    if (s != SPG_OK) {
        return s; /* could not read the journal at all */
    }

    uint8_t               want[SPG_HASH_BYTES];
    const enum spg_status hs =
        spg_hmac(key_n, key, SPG_JOURNAL_HASH_BYTES, head, SPG_HASH_BYTES, want);
    if (hs != SPG_OK) {
        return hs;
    }
    uint8_t got[SPG_HASH_BYTES];
    if (!read_hex_tag(sig_path, got)) {
        return SPG_OK; /* missing / garbled signature -> not verified */
    }
    *ok = spg_hmac_equal(want, got);
    return SPG_OK;
}
