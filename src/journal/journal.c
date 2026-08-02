#include "geistshell/journal.h"

#include "geistshell/hash.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

static void zero_hash(uint8_t hash[static SPG_JOURNAL_HASH_BYTES]) {
    memset(hash, 0, SPG_JOURNAL_HASH_BYTES);
}

static bool valid_event_kind(const enum spg_journal_event_kind kind) {
    return kind == SPG_JOURNAL_EVENT_RUN_START ||
           kind == SPG_JOURNAL_EVENT_POLICY_DECISION ||
           kind == SPG_JOURNAL_EVENT_MODEL_INPUT ||
           kind == SPG_JOURNAL_EVENT_MODEL_OUTPUT ||
           kind == SPG_JOURNAL_EVENT_ACTION ||
           kind == SPG_JOURNAL_EVENT_RESULT ||
           kind == SPG_JOURNAL_EVENT_GRAPH ||
           kind == SPG_JOURNAL_EVENT_MEMORY ||
           kind == SPG_JOURNAL_EVENT_SIM ||
           kind == SPG_JOURNAL_EVENT_ERROR;
}

static enum spg_status read_exact(FILE *file, void *data, size_t bytes);

static enum spg_status hash_record(
    const struct spg_journal_record_header *header, const size_t payload_n,
    const uint8_t payload[], uint8_t out[static SPG_JOURNAL_HASH_BYTES]) {
    if (header == nullptr || (payload_n > 0u && payload == nullptr) ||
        out == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    struct spg_journal_record_header hash_header = *header;
    zero_hash(hash_header.record_hash);

    struct spg_hash_state state = {};
    spg_hash_init(&state);
    enum spg_status status =
        spg_hash_update(&state, sizeof(hash_header), (const uint8_t *)&hash_header);
    if (status != SPG_OK) {
        return status;
    }
    status = spg_hash_update(&state, payload_n, payload);
    if (status != SPG_OK) {
        return status;
    }
    return spg_hash_final(&state, SPG_JOURNAL_HASH_BYTES, out);
}

static enum spg_status hash_record_from_file(
    FILE *file, const struct spg_journal_record_header *header,
    const size_t payload_capacity, uint8_t payload[],
    struct spg_journal_record *out, uint8_t computed[static SPG_JOURNAL_HASH_BYTES],
    bool *payload_fit) {
    if (file == nullptr || header == nullptr || out == nullptr ||
        computed == nullptr || payload_fit == nullptr ||
        (payload_capacity > 0u && payload == nullptr)) {
        return SPG_E_INVALID_ARG;
    }

    struct spg_journal_record_header hash_header = *header;
    zero_hash(hash_header.record_hash);

    struct spg_hash_state state = {};
    spg_hash_init(&state);
    enum spg_status status =
        spg_hash_update(&state, sizeof(hash_header), (const uint8_t *)&hash_header);
    if (status != SPG_OK) {
        return status;
    }

    const size_t payload_n = (size_t)header->payload_bytes;
    *payload_fit = payload_n <= payload_capacity;
    if (*payload_fit) {
        status = read_exact(file, payload, payload_n);
        if (status != SPG_OK) {
            return SPG_E_JOURNAL_CORRUPT;
        }
        status = spg_hash_update(&state, payload_n, payload);
        if (status != SPG_OK) {
            return status;
        }
    } else {
        uint8_t scratch[256];
        size_t  remaining = payload_n;
        while (remaining > 0u) {
            const size_t take =
                remaining < sizeof(scratch) ? remaining : sizeof(scratch);
            status = read_exact(file, scratch, take);
            if (status != SPG_OK) {
                return SPG_E_JOURNAL_CORRUPT;
            }
            status = spg_hash_update(&state, take, scratch);
            if (status != SPG_OK) {
                return status;
            }
            remaining -= take;
        }
    }

    status = spg_hash_final(&state, SPG_JOURNAL_HASH_BYTES, computed);
    if (status != SPG_OK) {
        return status;
    }
    out->header       = *header;
    out->payload_used = payload_n;
    return SPG_OK;
}

static enum spg_status write_exact(FILE *file, const void *data,
                                   const size_t bytes) {
    if (bytes == 0u) {
        return SPG_OK;
    }
    return fwrite(data, 1u, bytes, file) == bytes ? SPG_OK : SPG_E_IO;
}

static enum spg_status read_exact(FILE *file, void *data, const size_t bytes) {
    if (bytes == 0u) {
        return SPG_OK;
    }
    return fread(data, 1u, bytes, file) == bytes ? SPG_OK : SPG_E_IO;
}

enum spg_status spg_journal_writer_open(struct spg_journal_writer *writer,
                                        const char *path) {
    if (writer == nullptr || path == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *writer = (struct spg_journal_writer){};
    writer->file = fopen(path, "wb");
    if (writer->file == nullptr) {
        return SPG_E_IO;
    }
    writer->next_sequence = 1u;
    zero_hash(writer->last_hash);
    return SPG_OK;
}

enum spg_status spg_journal_writer_append(
    struct spg_journal_writer *writer, const uint64_t timestamp_ns,
    const uint64_t parent_sequence,
    const enum spg_journal_event_kind event_kind,
    const enum spg_status event_status, const size_t payload_n,
    const uint8_t payload[], uint64_t *out_sequence) {
    if (writer == nullptr || writer->file == nullptr ||
        (payload_n > 0u && payload == nullptr) || out_sequence == nullptr ||
        !valid_event_kind(event_kind)) {
        return SPG_E_INVALID_ARG;
    }
    if ((uint64_t)payload_n != payload_n) {
        return SPG_E_OVERFLOW;
    }

    struct spg_journal_record_header header = {
        .magic           = SPG_JOURNAL_MAGIC,
        .version         = SPG_JOURNAL_VERSION,
        .header_bytes    = (uint16_t)sizeof(struct spg_journal_record_header),
        .sequence        = writer->next_sequence,
        .timestamp_ns    = timestamp_ns,
        .parent_sequence = parent_sequence,
        .event_kind      = (uint32_t)event_kind,
        .status          = (uint32_t)event_status,
        .payload_bytes   = (uint64_t)payload_n,
    };
    memcpy(header.prev_hash, writer->last_hash, SPG_JOURNAL_HASH_BYTES);
    enum spg_status status =
        hash_record(&header, payload_n, payload, header.record_hash);
    if (status != SPG_OK) {
        return status;
    }

    status = write_exact(writer->file, &header, sizeof(header));
    if (status != SPG_OK) {
        return status;
    }
    status = write_exact(writer->file, payload, payload_n);
    if (status != SPG_OK) {
        return status;
    }
    if (fflush(writer->file) != 0) {
        return SPG_E_IO;
    }

    memcpy(writer->last_hash, header.record_hash, SPG_JOURNAL_HASH_BYTES);
    if (writer->header_log != nullptr &&
        writer->header_log_count < writer->header_log_capacity) {
        writer->header_log[writer->header_log_count] = header;
        writer->header_log_count += 1u;
    }
    *out_sequence = writer->next_sequence;
    writer->next_sequence += 1u;
    return SPG_OK;
}

void spg_journal_writer_set_header_log(
    struct spg_journal_writer *writer, const size_t capacity,
    struct spg_journal_record_header headers[]) {
    if (writer == nullptr) {
        return;
    }
    writer->header_log          = capacity > 0u ? headers : nullptr;
    writer->header_log_capacity = capacity;
    writer->header_log_count    = 0u;
}

enum spg_status spg_journal_writer_close(struct spg_journal_writer *writer) {
    if (writer == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    if (writer->file == nullptr) {
        return SPG_OK;
    }
    const int rc = fclose(writer->file);
    *writer      = (struct spg_journal_writer){};
    return rc == 0 ? SPG_OK : SPG_E_IO;
}

enum spg_status spg_journal_reader_open(struct spg_journal_reader *reader,
                                        const char *path) {
    if (reader == nullptr || path == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *reader = (struct spg_journal_reader){};
    reader->file = fopen(path, "rb");
    if (reader->file == nullptr) {
        return SPG_E_IO;
    }
    reader->next_sequence = 1u;
    zero_hash(reader->last_hash);
    return SPG_OK;
}

enum spg_status spg_journal_reader_next(struct spg_journal_reader *reader,
                                        const size_t payload_capacity,
                                        uint8_t payload[],
                                        struct spg_journal_record *out) {
    if (reader == nullptr || reader->file == nullptr || out == nullptr ||
        (payload_capacity > 0u && payload == nullptr)) {
        return SPG_E_INVALID_ARG;
    }
    *out = (struct spg_journal_record){};

    struct spg_journal_record_header header = {};
    const size_t nread = fread(&header, 1u, sizeof(header), reader->file);
    if (nread == 0u && feof(reader->file)) {
        return SPG_E_NOT_FOUND;
    }
    if (nread != sizeof(header)) {
        return SPG_E_JOURNAL_CORRUPT;
    }

    if (header.magic != SPG_JOURNAL_MAGIC ||
        header.version != SPG_JOURNAL_VERSION ||
        header.header_bytes != sizeof(struct spg_journal_record_header) ||
        header.sequence != reader->next_sequence ||
        !valid_event_kind((enum spg_journal_event_kind)header.event_kind)) {
        return SPG_E_JOURNAL_CORRUPT;
    }
    if (header.payload_bytes > SIZE_MAX) {
        return SPG_E_OVERFLOW;
    }
    if (memcmp(header.prev_hash, reader->last_hash, SPG_JOURNAL_HASH_BYTES) !=
        0) {
        return SPG_E_JOURNAL_CORRUPT;
    }

    uint8_t computed[SPG_JOURNAL_HASH_BYTES] = {};
    bool    payload_fit = false;
    enum spg_status status =
        hash_record_from_file(reader->file, &header, payload_capacity, payload,
                              out, computed, &payload_fit);
    if (status != SPG_OK) {
        return status;
    }
    if (memcmp(computed, header.record_hash, SPG_JOURNAL_HASH_BYTES) != 0) {
        return SPG_E_JOURNAL_CORRUPT;
    }

    memcpy(reader->last_hash, header.record_hash, SPG_JOURNAL_HASH_BYTES);
    reader->next_sequence += 1u;
    if (!payload_fit) {
        return SPG_E_LIMIT;
    }
    return SPG_OK;
}

enum spg_status spg_journal_reader_close(struct spg_journal_reader *reader) {
    if (reader == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    if (reader->file == nullptr) {
        return SPG_OK;
    }
    const int rc = fclose(reader->file);
    *reader      = (struct spg_journal_reader){};
    return rc == 0 ? SPG_OK : SPG_E_IO;
}
