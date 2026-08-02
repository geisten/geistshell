#ifndef GEISTSHELL_JOURNAL_H
#define GEISTSHELL_JOURNAL_H

#include "geistshell/status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPG_JOURNAL_MAGIC UINT32_C(0x53474a31)
#define SPG_JOURNAL_VERSION UINT16_C(1)
#define SPG_JOURNAL_HASH_BYTES 32u

enum spg_journal_event_kind {
    SPG_JOURNAL_EVENT_RUN_START = 0,
    SPG_JOURNAL_EVENT_POLICY_DECISION,
    SPG_JOURNAL_EVENT_MODEL_INPUT,
    SPG_JOURNAL_EVENT_MODEL_OUTPUT,
    SPG_JOURNAL_EVENT_ACTION,
    SPG_JOURNAL_EVENT_RESULT,
    SPG_JOURNAL_EVENT_GRAPH,
    SPG_JOURNAL_EVENT_MEMORY,
    SPG_JOURNAL_EVENT_SIM,
    SPG_JOURNAL_EVENT_ERROR,
};

struct spg_journal_record_header {
    uint32_t magic;
    uint16_t version;
    uint16_t header_bytes;
    uint64_t sequence;
    uint64_t timestamp_ns;
    uint64_t parent_sequence;
    uint32_t event_kind;
    uint32_t status;
    uint64_t payload_bytes;
    uint8_t  prev_hash[SPG_JOURNAL_HASH_BYTES];
    uint8_t  record_hash[SPG_JOURNAL_HASH_BYTES];
};

struct spg_journal_writer {
    FILE    *file;
    uint64_t next_sequence;
    uint8_t  last_hash[SPG_JOURNAL_HASH_BYTES];

    /* Optional in-memory log of every header written, for callers (e.g. the
     * agent loop) that feed the trajectory back into context without re-reading
     * the file. Null = disabled; capped, drops once full. */
    struct spg_journal_record_header *header_log;
    size_t                            header_log_capacity;
    size_t                            header_log_count;
};

struct spg_journal_reader {
    FILE    *file;
    uint64_t next_sequence;
    uint8_t  last_hash[SPG_JOURNAL_HASH_BYTES];
};

struct spg_journal_record {
    struct spg_journal_record_header header;
    size_t                           payload_used;
};

[[nodiscard]] enum spg_status
spg_journal_writer_open(struct spg_journal_writer *writer, const char *path);
[[nodiscard]] enum spg_status
spg_journal_writer_append(struct spg_journal_writer *writer,
                          uint64_t timestamp_ns, uint64_t parent_sequence,
                          enum spg_journal_event_kind event_kind,
                          enum spg_status event_status, size_t payload_n,
                          const uint8_t payload[],
                          uint64_t *out_sequence);
[[nodiscard]] enum spg_status
spg_journal_writer_close(struct spg_journal_writer *writer);

/* Bind a caller-owned array that receives a copy of every header subsequently
 * written (count reset to 0). Pass capacity 0 / null to disable. */
void spg_journal_writer_set_header_log(
    struct spg_journal_writer *writer, size_t capacity,
    struct spg_journal_record_header headers[]);

[[nodiscard]] enum spg_status
spg_journal_reader_open(struct spg_journal_reader *reader, const char *path);
[[nodiscard]] enum spg_status
spg_journal_reader_next(struct spg_journal_reader *reader,
                        size_t payload_capacity,
                        uint8_t payload[],
                        struct spg_journal_record *out);
[[nodiscard]] enum spg_status
spg_journal_reader_close(struct spg_journal_reader *reader);

/* ---- Sealing (keyed tamper-evidence) ----
 * The hash chain already makes every record's hash depend on all prior records,
 * so the final record hash commits to the whole log. Sealing writes an HMAC tag
 * over that final hash, keyed with a secret. An attacker who edits a record and
 * re-chains every hash still cannot reproduce the tag without the key; a holder
 * of the key can verify the log is intact and authentic. Symmetric — this is
 * tamper-evidence, not third-party non-repudiation. */

/* Verify the chain, then write the HMAC tag over the final chain hash to
 * sig_path (64 lowercase hex chars + newline). SPG_E_JOURNAL_CORRUPT if the
 * chain is broken; SPG_E_IO on write failure. */
[[nodiscard]] enum spg_status spg_journal_seal(const char *journal_path,
                                               const char *sig_path,
                                               size_t key_n,
                                               const uint8_t key[]);

/* Re-verify the chain and the tag in sig_path against the key; *ok receives the
 * verdict (false on a broken chain, a missing/garbled sig, or a tag mismatch).
 * Returns SPG_OK once the check ran; an error only on journal read failure. */
[[nodiscard]] enum spg_status spg_journal_verify_signed(const char *journal_path,
                                                        const char *sig_path,
                                                        size_t key_n,
                                                        const uint8_t key[],
                                                        bool *ok);

#ifdef __cplusplus
}
#endif

#endif
