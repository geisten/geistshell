/* Modbus TCP client and the channel table it drives.
 *
 * Split in three parts, in order of how testable they are: the table (pure),
 * the wire codec (pure), the socket (not). Everything that decides anything
 * lives in the first two. */

#include "geistshell/device.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* --- The table -------------------------------------------------------- */

void spg_device_init(struct spg_device *dev) {
    if (dev == nullptr) {
        return;
    }
    *dev = (struct spg_device){.fd = -1, .txn = 0u, .n_channels = 0u};
}

enum spg_status
spg_device_add_channel(struct spg_device               *dev,
                       const struct spg_device_channel *channel) {
    if (dev == nullptr || channel == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    if (channel->name[0] == '\0' || channel->min > channel->max) {
        return SPG_E_INVALID_ARG;
    }
    if (channel->writable &&
        (channel->safe < channel->min || channel->safe > channel->max)) {
        /* A safe value the channel cannot accept is not a fallback — the
         * watchdog would fire and then be refused by the range check. */
        return SPG_E_INVALID_ARG;
    }
    if (spg_device_find(dev, channel->name) != nullptr) {
        /* Two channels under one name means a write could reach either
         * register depending on table order. Refuse the table instead. */
        return SPG_E_INVALID_ARG;
    }
    if (dev->n_channels >= SPG_DEVICE_MAX_CHANNELS) {
        return SPG_E_LIMIT;
    }
    dev->channels[dev->n_channels] = *channel;
    dev->n_channels += 1u;
    return SPG_OK;
}

const struct spg_device_channel *spg_device_find(const struct spg_device *dev,
                                                 const char *name) {
    if (dev == nullptr || name == nullptr) {
        return nullptr;
    }
    for (size_t i = 0u; i < dev->n_channels; i += 1u) {
        if (strcmp(dev->channels[i].name, name) == 0) {
            return &dev->channels[i];
        }
    }
    return nullptr;
}

/* Read one ':'-delimited field as a signed integer. Returns the offset just
 * past the field, or 0 on malformed input. */
static size_t scan_int(size_t n, const char text[], size_t at, int64_t *out) {
    if (at >= n) {
        return 0u;
    }
    bool negative = false;
    if (text[at] == '-') {
        negative = true;
        at += 1u;
    }
    int64_t value = 0;
    size_t  start = at;
    for (; at < n && text[at] >= '0' && text[at] <= '9'; at += 1u) {
        if (value > (INT64_MAX - (text[at] - '0')) / 10) {
            return 0u; /* an overflowing bound is not a bound */
        }
        value = value * 10 + (text[at] - '0');
    }
    if (at == start) {
        return 0u;
    }
    *out = negative ? -value : value;
    return at;
}

static size_t skip_colon(size_t n, const char text[], size_t at) {
    return (at < n && text[at] == ':') ? at + 1u : 0u;
}

enum spg_status spg_device_parse_channel(const size_t text_n, const char text[],
                                         struct spg_device_channel *out) {
    if (out == nullptr || (text_n > 0u && text == nullptr)) {
        return SPG_E_INVALID_ARG;
    }
    *out = (struct spg_device_channel){};

    size_t at = 0u;
    while (at < text_n && text[at] != ':') {
        if (at + 1u >= SPG_DEVICE_NAME_CAP) {
            return SPG_E_LIMIT;
        }
        out->name[at] = text[at];
        at += 1u;
    }
    if (at == 0u) {
        return SPG_E_FORMAT; /* an unnamed channel cannot be addressed */
    }
    out->name[at] = '\0';

    int64_t reg = 0, min = 0, max = 0;
    at = skip_colon(text_n, text, at);
    if (at == 0u || (at = scan_int(text_n, text, at, &reg)) == 0u) {
        return SPG_E_FORMAT;
    }
    at = skip_colon(text_n, text, at);
    if (at == 0u || (at = scan_int(text_n, text, at, &min)) == 0u) {
        return SPG_E_FORMAT;
    }
    at = skip_colon(text_n, text, at);
    if (at == 0u || (at = scan_int(text_n, text, at, &max)) == 0u) {
        return SPG_E_FORMAT;
    }
    at = skip_colon(text_n, text, at);
    if (at == 0u || at >= text_n) {
        return SPG_E_FORMAT;
    }
    if (text[at] == 'w') {
        out->writable = true;
    } else if (text[at] != 'r') {
        return SPG_E_FORMAT;
    }
    at += 1u;

    if (out->writable) {
        /* Required, not optional. A writable channel with no declared safe
         * value leaves the loss-of-contact decision to whatever the machine
         * was last told, which is the state least likely to be safe. */
        int64_t safe = 0;
        at           = skip_colon(text_n, text, at);
        if (at == 0u || (at = scan_int(text_n, text, at, &safe)) == 0u) {
            return SPG_E_FORMAT;
        }
        out->safe = safe;
    }
    if (at != text_n) {
        return SPG_E_FORMAT; /* trailing text means the operator meant something
                                this parser did not understand */
    }
    if (reg < 0 || reg > UINT16_MAX || min > max) {
        return SPG_E_FORMAT;
    }
    if (out->writable && (out->safe < min || out->safe > max)) {
        return SPG_E_FORMAT;
    }
    out->reg = (uint16_t)reg;
    out->min = min;
    out->max = max;
    return SPG_OK;
}

/* --- The wire --------------------------------------------------------- */

static void put_u16(uint8_t out[static 2], const uint16_t value) {
    /* Written byte by byte rather than by casting a uint16_t*: Modbus is
     * big-endian on the wire regardless of the host, and a cast would make the
     * frame depend on the CPU it was built for. */
    out[0] = (uint8_t)(value >> 8);
    out[1] = (uint8_t)(value & 0xffu);
}

static uint16_t get_u16(const uint8_t in[static 2]) {
    return (uint16_t)(((uint16_t)in[0] << 8) | (uint16_t)in[1]);
}

/* MBAP header: transaction(2) protocol(2) length(2) unit(1), then the PDU. */
static size_t encode(const uint16_t txn, const uint16_t unit,
                     const uint8_t function, const uint16_t reg,
                     const uint16_t arg, uint8_t out[static 12]) {
    put_u16(&out[0], txn);
    put_u16(&out[2], 0u); /* protocol id: always 0 for Modbus */
    put_u16(&out[4], 6u); /* length: unit + function + two 16-bit fields */
    out[6] = (uint8_t)unit;
    out[7] = function;
    put_u16(&out[8], reg);
    put_u16(&out[10], arg);
    return 12u;
}

size_t spg_modbus_encode_read(const uint16_t txn, const uint16_t unit,
                              const uint16_t reg, uint8_t out[static 12]) {
    return encode(txn, unit, 0x03u, reg, 1u, out); /* one register */
}

size_t spg_modbus_encode_write(const uint16_t txn, const uint16_t unit,
                               const uint16_t reg, const uint16_t value,
                               uint8_t out[static 12]) {
    return encode(txn, unit, 0x06u, reg, value, out);
}

enum spg_status spg_modbus_decode(const size_t n, const uint8_t frame[],
                                  const uint16_t txn, uint16_t *out_value) {
    if (out_value == nullptr || (n > 0u && frame == nullptr)) {
        return SPG_E_INVALID_ARG;
    }
    if (n < 9u) {
        return SPG_E_FORMAT;
    }
    if (get_u16(&frame[0]) != txn) {
        /* Somebody else's answer. On a bus fronted by a gateway this is how one
         * machine's reading gets attributed to another. */
        return SPG_E_REPLAY_MISMATCH;
    }
    if (get_u16(&frame[2]) != 0u) {
        return SPG_E_FORMAT;
    }
    const uint8_t function = frame[7];
    if ((function & 0x80u) != 0u) {
        return SPG_E_IO; /* the device refused; frame[8] carries its code */
    }
    if (function == 0x03u) {
        if (n < 11u || frame[8] != 2u) {
            return SPG_E_FORMAT;
        }
        *out_value = get_u16(&frame[9]);
        return SPG_OK;
    }
    if (function == 0x06u) {
        if (n < 12u) {
            return SPG_E_FORMAT;
        }
        *out_value = get_u16(&frame[10]); /* echo of the written value */
        return SPG_OK;
    }
    return SPG_E_UNSUPPORTED;
}

/* --- The watchdog ----------------------------------------------------- */

void spg_device_arm_watchdog(struct spg_device *dev, const uint64_t timeout,
                             const uint64_t now) {
    if (dev == nullptr) {
        return;
    }
    dev->watchdog_timeout = timeout;
    dev->last_contact     = now;
    dev->contact_pending  = false;
}

enum spg_device_watchdog spg_device_watchdog_check(struct spg_device *dev,
                                                   const uint64_t     now) {
    if (dev == nullptr) {
        return SPG_WATCHDOG_DISABLED;
    }
    if (dev->contact_pending) {
        dev->contact_pending = false;
        dev->last_contact    = now;
    }
    if (dev->watchdog_timeout == 0u) {
        return SPG_WATCHDOG_DISABLED;
    }
    if (now < dev->last_contact) {
        /* Time moved backwards. Treated as contact rather than as expiry: an
         * injected timestamp going backwards is a caller bug, and tripping a
         * machine into its safe state on a bookkeeping error would be the more
         * expensive of the two wrong answers. */
        return SPG_WATCHDOG_OK;
    }
    return (now - dev->last_contact) > dev->watchdog_timeout
               ? SPG_WATCHDOG_EXPIRED
               : SPG_WATCHDOG_OK;
}

enum spg_status spg_device_safe_state(struct spg_device *dev) {
    if (dev == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    enum spg_status first_failure = SPG_OK;
    for (size_t i = 0u; i < dev->n_channels; i += 1u) {
        if (!dev->channels[i].writable) {
            continue;
        }
        const enum spg_status status =
            spg_device_write(dev, dev->channels[i].name, dev->channels[i].safe);
        if (status != SPG_OK && first_failure == SPG_OK) {
            first_failure = status; /* keep going: see device.h */
        }
    }
    return first_failure;
}

/* --- The machine ------------------------------------------------------ */

enum spg_status spg_device_connect(struct spg_device *dev, const char *host,
                                   const uint16_t port) {
    if (dev == nullptr || host == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    if (dev->fd >= 0) {
        return SPG_E_INVALID_STATE;
    }
    char port_text[6] = {};
    (void)snprintf(port_text, sizeof port_text, "%u", (unsigned)port);

    struct addrinfo  hints = {.ai_family   = AF_UNSPEC,
                              .ai_socktype = SOCK_STREAM};
    struct addrinfo *list  = nullptr;
    if (getaddrinfo(host, port_text, &hints, &list) != 0) {
        return SPG_E_IO;
    }
    enum spg_status status = SPG_E_IO;
    for (const struct addrinfo *it = list; it != nullptr; it = it->ai_next) {
        const int fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) {
            continue;
        }
        /* Both directions bounded. A machine that accepts the connection and
         * then goes quiet is the failure this guards: without it the governing
         * loop blocks in read() and stops governing anything. */
        struct timeval tv = {.tv_sec  = SPG_DEVICE_TIMEOUT_MS / 1000,
                             .tv_usec = (SPG_DEVICE_TIMEOUT_MS % 1000) * 1000};
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        if (connect(fd, it->ai_addr, it->ai_addrlen) == 0) {
            dev->fd = fd;
            status  = SPG_OK;
            break;
        }
        (void)close(fd);
    }
    freeaddrinfo(list);
    return status;
}

void spg_device_close(struct spg_device *dev) {
    if (dev != nullptr && dev->fd >= 0) {
        (void)close(dev->fd);
        dev->fd = -1;
    }
}

static enum spg_status write_all(const int fd, const size_t n,
                                 const uint8_t buf[]) {
    size_t sent = 0u;
    while (sent < n) {
        const ssize_t got = send(fd, &buf[sent], n - sent, 0);
        if (got <= 0) {
            if (got < 0 && errno == EINTR) {
                continue;
            }
            return SPG_E_IO;
        }
        sent += (size_t)got;
    }
    return SPG_OK;
}

static enum spg_status read_exact(const int fd, const size_t n, uint8_t buf[]) {
    size_t have = 0u;
    while (have < n) {
        const ssize_t got = recv(fd, &buf[have], n - have, 0);
        if (got <= 0) {
            if (got < 0 && errno == EINTR) {
                continue;
            }
            return SPG_E_IO; /* covers the timeout: a silent machine is an
                                unreachable machine, not a slow one */
        }
        have += (size_t)got;
    }
    return SPG_OK;
}

/* One request, one response. The MBAP length field says how much follows, so
 * the reply is read in two steps rather than guessed at. */
static enum spg_status transact(struct spg_device *dev, const size_t n,
                                const uint8_t request[], const uint16_t txn,
                                uint16_t *out_value) {
    enum spg_status status = write_all(dev->fd, n, request);
    if (status != SPG_OK) {
        return status;
    }
    uint8_t reply[SPG_MODBUS_FRAME_CAP] = {};
    status                              = read_exact(dev->fd, 6u, reply);
    if (status != SPG_OK) {
        return status;
    }
    const uint16_t remaining = get_u16(&reply[4]);
    if (remaining == 0u || remaining > SPG_MODBUS_FRAME_CAP - 6u) {
        return SPG_E_FORMAT;
    }
    status = read_exact(dev->fd, remaining, &reply[6]);
    if (status != SPG_OK) {
        return status;
    }
    status = spg_modbus_decode((size_t)remaining + 6u, reply, txn, out_value);
    if (status == SPG_OK) {
        /* Set here rather than in read/write so no future caller can add a
         * transaction that talks to the machine without counting as contact. */
        dev->contact_pending = true;
    }
    return status;
}

enum spg_status spg_device_read(struct spg_device *dev, const char *name,
                                int64_t *out) {
    if (dev == nullptr || out == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    if (dev->fd < 0) {
        return SPG_E_INVALID_STATE;
    }
    const struct spg_device_channel *channel = spg_device_find(dev, name);
    if (channel == nullptr) {
        return SPG_E_NOT_FOUND;
    }
    dev->txn += 1u;
    uint8_t      request[SPG_MODBUS_FRAME_CAP] = {};
    const size_t n =
        spg_modbus_encode_read(dev->txn, channel->unit, channel->reg, request);
    uint16_t              value  = 0u;
    const enum spg_status status = transact(dev, n, request, dev->txn, &value);
    if (status != SPG_OK) {
        return status;
    }
    /* Registers carry signed measurements as two's complement — a below-zero
     * temperature is the common case that a plain unsigned read gets wrong by
     * 65536. */
    *out = (int64_t)(int16_t)value;
    return SPG_OK;
}

enum spg_status spg_device_write(struct spg_device *dev, const char *name,
                                 const int64_t value) {
    if (dev == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    /* Every refusal is decided BEFORE the connection is consulted, so all of
     * it can be tested without a socket. A safety check reachable only once a
     * machine is plugged in is a safety check nobody runs. */
    const struct spg_device_channel *channel = spg_device_find(dev, name);
    if (channel == nullptr) {
        return SPG_E_NOT_FOUND;
    }
    if (!channel->writable) {
        return SPG_E_POLICY_DENIED;
    }
    if (value < channel->min || value > channel->max) {
        return SPG_E_LIMIT; /* refused, not clamped — see device.h */
    }
    if (value < INT16_MIN || value > INT16_MAX) {
        return SPG_E_OVERFLOW;
    }
    if (dev->fd < 0) {
        return SPG_E_INVALID_STATE;
    }
    dev->txn += 1u;
    uint8_t      request[SPG_MODBUS_FRAME_CAP] = {};
    const size_t n =
        spg_modbus_encode_write(dev->txn, channel->unit, channel->reg,
                                (uint16_t)(int16_t)value, request);
    uint16_t              echo   = 0u;
    const enum spg_status status = transact(dev, n, request, dev->txn, &echo);
    if (status != SPG_OK) {
        return status;
    }
    if ((int64_t)(int16_t)echo != value) {
        /* The device acknowledged a different value than it was told. Treated
         * as a failure: a caller that believes its write landed will build the
         * next decision on a number the machine never accepted. */
        return SPG_E_IO;
    }
    return SPG_OK;
}

/* --- What the agent sees ----------------------------------------------- */

enum spg_status spg_device_sample(struct spg_device       *dev,
                                  struct spg_device_state *out) {
    if (dev == nullptr || out == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out = (struct spg_device_state){};
    enum spg_status first = SPG_OK;
    for (size_t i = 0u; i < dev->n_channels; i += 1u) {
        struct spg_device_reading *reading = &out->readings[out->n];
        memcpy(reading->name, dev->channels[i].name, sizeof reading->name);
        int64_t               value  = 0;
        const enum spg_status status = spg_device_read(dev, reading->name,
                                                       &value);
        if (status == SPG_OK) {
            reading->value = value;
            reading->known = true;
        } else if (first == SPG_OK) {
            /* Every channel is attempted even after one fails — one dead
             * sensor must not blind the agent to the rest of the plant. The
             * first failure is what the caller hears about. */
            first = status;
        }
        out->n += 1u;
    }
    return first;
}

enum spg_status spg_device_state_render(const struct spg_device_state *state,
                                        const size_t dst_capacity,
                                        char dst[static dst_capacity],
                                        size_t *out_required) {
    if (state == nullptr || dst == nullptr || out_required == nullptr ||
        dst_capacity == 0u || state->n > SPG_DEVICE_MAX_CHANNELS) {
        return SPG_E_INVALID_ARG;
    }
    /* SPG_DEVICE_RENDER_CAP bounds the worst case by construction, so the
     * whole block is rendered here first and copied only if it fits — no
     * partial record ever reaches dst. */
    char   block[SPG_DEVICE_RENDER_CAP];
    size_t used = (size_t)snprintf(block, sizeof block, "(device-state");
    for (size_t i = 0u; i < state->n; i += 1u) {
        const struct spg_device_reading *r = &state->readings[i];
        used += r->known
                    ? (size_t)snprintf(block + used, sizeof block - used,
                                       " (%s %lld)", r->name,
                                       (long long)r->value)
                    /* Never 0 for a reading that was not taken — device.h. */
                    : (size_t)snprintf(block + used, sizeof block - used,
                                       " (%s unknown)", r->name);
    }
    used += (size_t)snprintf(block + used, sizeof block - used, ")");
    *out_required = used + 1u;
    if (used + 1u > dst_capacity) {
        return SPG_E_LIMIT; /* no partial record: dst is left untrusted */
    }
    memcpy(dst, block, used + 1u);
    return SPG_OK;
}
