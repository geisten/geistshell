#include "geistshell/journal.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *journal_path = "/tmp/geistshell_test_journal.sgj";

static int write_sample_journal(void) {
    struct spg_journal_writer writer = {};
    const uint8_t             p1[]   = {'o', 'n', 'e'};
    const uint8_t             p2[]   = {'t', 'w', 'o', '!'};
    uint64_t                  seq    = 0u;

    unlink(journal_path);
    if (spg_journal_writer_open(&writer, journal_path) != SPG_OK) {
        return 1;
    }
    if (spg_journal_writer_append(&writer, 10u, 0u,
                                  SPG_JOURNAL_EVENT_RUN_START, SPG_OK,
                                  sizeof(p1), p1, &seq) != SPG_OK ||
        seq != 1u) {
        (void)spg_journal_writer_close(&writer);
        return 1;
    }
    if (spg_journal_writer_append(&writer, 20u, 1u,
                                  SPG_JOURNAL_EVENT_POLICY_DECISION,
                                  SPG_E_POLICY_DENIED, sizeof(p2), p2,
                                  &seq) != SPG_OK ||
        seq != 2u) {
        (void)spg_journal_writer_close(&writer);
        return 1;
    }
    return spg_journal_writer_close(&writer) == SPG_OK ? 0 : 1;
}

static int test_write_read_roundtrip(void) {
    struct spg_journal_reader reader  = {};
    struct spg_journal_record record  = {};
    uint8_t                   payload[8];

    if (write_sample_journal() != 0) {
        return 1;
    }
    if (spg_journal_reader_open(&reader, journal_path) != SPG_OK) {
        return 1;
    }
    if (spg_journal_reader_next(&reader, sizeof(payload), payload, &record) !=
        SPG_OK) {
        (void)spg_journal_reader_close(&reader);
        return 1;
    }
    if (record.header.sequence != 1u || record.header.timestamp_ns != 10u ||
        record.header.event_kind != SPG_JOURNAL_EVENT_RUN_START ||
        record.payload_used != 3u || memcmp(payload, "one", 3u) != 0) {
        (void)spg_journal_reader_close(&reader);
        return 1;
    }
    if (spg_journal_reader_next(&reader, sizeof(payload), payload, &record) !=
        SPG_OK) {
        (void)spg_journal_reader_close(&reader);
        return 1;
    }
    if (record.header.sequence != 2u || record.header.parent_sequence != 1u ||
        record.header.status != SPG_E_POLICY_DENIED ||
        record.payload_used != 4u || memcmp(payload, "two!", 4u) != 0) {
        (void)spg_journal_reader_close(&reader);
        return 1;
    }
    if (spg_journal_reader_next(&reader, sizeof(payload), payload, &record) !=
        SPG_E_NOT_FOUND) {
        (void)spg_journal_reader_close(&reader);
        return 1;
    }
    return spg_journal_reader_close(&reader) == SPG_OK ? 0 : 1;
}

static int test_payload_limit_skips_record(void) {
    struct spg_journal_reader reader = {};
    struct spg_journal_record record = {};
    uint8_t                   payload[3];

    if (write_sample_journal() != 0) {
        return 1;
    }
    if (spg_journal_reader_open(&reader, journal_path) != SPG_OK) {
        return 1;
    }
    if (spg_journal_reader_next(&reader, sizeof(payload), payload, &record) !=
        SPG_OK) {
        (void)spg_journal_reader_close(&reader);
        return 1;
    }
    if (spg_journal_reader_next(&reader, sizeof(payload), payload, &record) !=
        SPG_E_LIMIT) {
        (void)spg_journal_reader_close(&reader);
        return 1;
    }
    if (record.header.sequence != 2u || record.payload_used != 4u) {
        (void)spg_journal_reader_close(&reader);
        return 1;
    }
    if (spg_journal_reader_next(&reader, sizeof(payload), payload, &record) !=
        SPG_E_NOT_FOUND) {
        (void)spg_journal_reader_close(&reader);
        return 1;
    }
    return spg_journal_reader_close(&reader) == SPG_OK ? 0 : 1;
}

static int test_hash_chain_fields(void) {
    struct spg_journal_reader reader = {};
    struct spg_journal_record first  = {};
    struct spg_journal_record second = {};
    uint8_t                   payload[8];

    if (write_sample_journal() != 0) {
        return 1;
    }
    if (spg_journal_reader_open(&reader, journal_path) != SPG_OK) {
        return 1;
    }
    if (spg_journal_reader_next(&reader, sizeof(payload), payload, &first) !=
        SPG_OK) {
        (void)spg_journal_reader_close(&reader);
        return 1;
    }
    if (spg_journal_reader_next(&reader, sizeof(payload), payload, &second) !=
        SPG_OK) {
        (void)spg_journal_reader_close(&reader);
        return 1;
    }
    (void)spg_journal_reader_close(&reader);

    uint8_t zero_hash[SPG_JOURNAL_HASH_BYTES] = {};
    if (memcmp(first.header.record_hash, zero_hash, SPG_JOURNAL_HASH_BYTES) ==
        0) {
        return 1;
    }
    if (memcmp(first.header.prev_hash, zero_hash, SPG_JOURNAL_HASH_BYTES) !=
        0) {
        return 1;
    }
    if (memcmp(second.header.prev_hash, first.header.record_hash,
               SPG_JOURNAL_HASH_BYTES) != 0) {
        return 1;
    }
    return 0;
}

static int test_payload_tamper_detected(void) {
    if (write_sample_journal() != 0) {
        return 1;
    }

    FILE *file = fopen(journal_path, "r+b");
    if (file == nullptr) {
        return 1;
    }
    const long first_payload_offset =
        (long)sizeof(struct spg_journal_record_header);
    if (fseek(file, first_payload_offset, SEEK_SET) != 0) {
        (void)fclose(file);
        return 1;
    }
    const uint8_t bad = 'x';
    if (fwrite(&bad, 1u, 1u, file) != 1u) {
        (void)fclose(file);
        return 1;
    }
    if (fclose(file) != 0) {
        return 1;
    }

    struct spg_journal_reader reader = {};
    struct spg_journal_record record = {};
    uint8_t                   payload[8];
    if (spg_journal_reader_open(&reader, journal_path) != SPG_OK) {
        return 1;
    }
    const enum spg_status status =
        spg_journal_reader_next(&reader, sizeof(payload), payload, &record);
    (void)spg_journal_reader_close(&reader);
    return status == SPG_E_JOURNAL_CORRUPT ? 0 : 1;
}

static int test_corrupt_magic(void) {
    FILE *file = fopen(journal_path, "wb");
    if (file == nullptr) {
        return 1;
    }
    struct spg_journal_record_header header = {
        .magic        = 0u,
        .version      = SPG_JOURNAL_VERSION,
        .header_bytes = (uint16_t)sizeof(struct spg_journal_record_header),
        .sequence     = 1u,
        .event_kind   = SPG_JOURNAL_EVENT_RUN_START,
    };
    if (fwrite(&header, 1u, sizeof(header), file) != sizeof(header)) {
        (void)fclose(file);
        return 1;
    }
    if (fclose(file) != 0) {
        return 1;
    }

    struct spg_journal_reader reader = {};
    struct spg_journal_record record = {};
    uint8_t                   payload[1];
    if (spg_journal_reader_open(&reader, journal_path) != SPG_OK) {
        return 1;
    }
    const enum spg_status status =
        spg_journal_reader_next(&reader, sizeof(payload), payload, &record);
    (void)spg_journal_reader_close(&reader);
    return status == SPG_E_JOURNAL_CORRUPT ? 0 : 1;
}

static int test_truncated_header(void) {
    FILE *file = fopen(journal_path, "wb");
    if (file == nullptr) {
        return 1;
    }
    const uint32_t partial = SPG_JOURNAL_MAGIC;
    if (fwrite(&partial, 1u, sizeof(partial), file) != sizeof(partial)) {
        (void)fclose(file);
        return 1;
    }
    if (fclose(file) != 0) {
        return 1;
    }

    struct spg_journal_reader reader = {};
    struct spg_journal_record record = {};
    uint8_t                   payload[1];
    if (spg_journal_reader_open(&reader, journal_path) != SPG_OK) {
        return 1;
    }
    const enum spg_status status =
        spg_journal_reader_next(&reader, sizeof(payload), payload, &record);
    (void)spg_journal_reader_close(&reader);
    return status == SPG_E_JOURNAL_CORRUPT ? 0 : 1;
}

static int test_invalid_args(void) {
    struct spg_journal_writer writer = {};
    struct spg_journal_reader reader = {};
    struct spg_journal_record record = {};
    uint64_t                  seq    = 0u;
    uint8_t                   payload[1] = {0u};

    if (spg_journal_writer_open(nullptr, journal_path) != SPG_E_INVALID_ARG) {
        return 1;
    }
    if (spg_journal_writer_append(&writer, 0u, 0u,
                                  SPG_JOURNAL_EVENT_RUN_START, SPG_OK, 1u,
                                  payload, &seq) != SPG_E_INVALID_ARG) {
        return 1;
    }
    if (spg_journal_reader_open(nullptr, journal_path) != SPG_E_INVALID_ARG) {
        return 1;
    }
    if (spg_journal_reader_next(&reader, 1u, payload, &record) !=
        SPG_E_INVALID_ARG) {
        return 1;
    }
    return 0;
}

static int test_header_log(void) {
    struct spg_journal_writer        writer = {};
    struct spg_journal_record_header log[4];
    const uint8_t                    p[] = {'x'};
    uint64_t                         seq = 0u;

    unlink(journal_path);
    if (spg_journal_writer_open(&writer, journal_path) != SPG_OK) {
        return 1;
    }
    spg_journal_writer_set_header_log(&writer, 2u, log); /* cap 2 */
    for (int i = 0; i < 3; i += 1) {
        if (spg_journal_writer_append(&writer, 10u, 0u, SPG_JOURNAL_EVENT_ACTION,
                                      SPG_OK, sizeof p, p, &seq) != SPG_OK) {
            (void)spg_journal_writer_close(&writer);
            return 1;
        }
    }
    /* Logged the first two headers (capped), in order, with their sequences. */
    const int ok = (writer.header_log_count == 2u && log[0].sequence == 1u &&
                    log[0].event_kind == (uint32_t)SPG_JOURNAL_EVENT_ACTION &&
                    log[1].sequence == 2u)
                       ? 0
                       : 1;
    (void)spg_journal_writer_close(&writer);
    return ok;
}

int main(void) {
    if (test_header_log() != 0) {
        fprintf(stderr, "test_header_log failed\n");
        return 1;
    }
    if (test_write_read_roundtrip() != 0) {
        fprintf(stderr, "test_write_read_roundtrip failed\n");
        return 1;
    }
    if (test_payload_limit_skips_record() != 0) {
        fprintf(stderr, "test_payload_limit_skips_record failed\n");
        return 1;
    }
    if (test_hash_chain_fields() != 0) {
        fprintf(stderr, "test_hash_chain_fields failed\n");
        return 1;
    }
    if (test_payload_tamper_detected() != 0) {
        fprintf(stderr, "test_payload_tamper_detected failed\n");
        return 1;
    }
    if (test_corrupt_magic() != 0) {
        fprintf(stderr, "test_corrupt_magic failed\n");
        return 1;
    }
    if (test_truncated_header() != 0) {
        fprintf(stderr, "test_truncated_header failed\n");
        return 1;
    }
    if (test_invalid_args() != 0) {
        fprintf(stderr, "test_invalid_args failed\n");
        return 1;
    }
    unlink(journal_path);
    return 0;
}
