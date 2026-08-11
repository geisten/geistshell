/* Everything here runs without a socket. That is the design constraint, not a
 * convenience: the refusals below are the safety layer, and a safety layer
 * that needs a machine plugged in to be exercised is one that gets tested on
 * the day it is needed. */

#include "geistshell/device.h"

#include <stdio.h>
#include <string.h>

#define LIT(s) (sizeof(s) - 1u), (s)

static int test_parse_channel(void) {
    struct spg_device_channel channel = {};

    if (spg_device_parse_channel(LIT("heater:1:0:100:w"), &channel) != SPG_OK) {
        return 1;
    }
    if (strcmp(channel.name, "heater") != 0 || channel.reg != 1u ||
        channel.min != 0 || channel.max != 100 || !channel.writable) {
        return 1;
    }

    if (spg_device_parse_channel(LIT("temp:0:-400:9000:r"), &channel) !=
            SPG_OK ||
        channel.min != -400 || channel.writable) {
        return 1;
    }

    /* Rejected: no name to address, a range that is inside out, a mode that is
     * neither r nor w, a missing field, and trailing text. The last one
     * matters most — "heater:1:0:100:write" silently parsing as writable would
     * accept an operator's typo as permission. */
    const char *bad[] = {
        ":1:0:100:w",           "heater:1:100:0:w",       "heater:1:0:100:x",
        "heater:1:0:w",         "heater:1:0:100:w extra", "heater:1:0:100:",
        "heater:70000:0:100:w",
    };
    for (size_t i = 0u; i < sizeof bad / sizeof bad[0]; i += 1u) {
        if (spg_device_parse_channel(strlen(bad[i]), bad[i], &channel) ==
            SPG_OK) {
            return 1;
        }
    }
    return 0;
}

static int test_table(void) {
    struct spg_device dev = {};
    spg_device_init(&dev);
    if (dev.fd != -1 || dev.n_channels != 0u) {
        return 1;
    }

    struct spg_device_channel heater = {
        .name = "heater", .reg = 1u, .min = 0, .max = 100, .writable = true};
    struct spg_device_channel temp = {
        .name = "temp", .reg = 0u, .min = -400, .max = 9000};

    if (spg_device_add_channel(&dev, &heater) != SPG_OK ||
        spg_device_add_channel(&dev, &temp) != SPG_OK) {
        return 1;
    }
    /* A duplicate name would make a write land on whichever register the table
     * happened to list first. */
    if (spg_device_add_channel(&dev, &heater) == SPG_OK) {
        return 1;
    }
    if (spg_device_find(&dev, "heater") == nullptr ||
        spg_device_find(&dev, "nope") != nullptr) {
        return 1;
    }

    struct spg_device full = {};
    spg_device_init(&full);
    for (size_t i = 0u; i < SPG_DEVICE_MAX_CHANNELS; i += 1u) {
        struct spg_device_channel ch = {.reg = (uint16_t)i, .max = 1};
        ch.name[0]                   = 'a';
        ch.name[1]                   = (char)('0' + (int)(i % 10u));
        ch.name[2]                   = (char)('0' + (int)(i / 10u));
        if (spg_device_add_channel(&full, &ch) != SPG_OK) {
            return 1;
        }
    }
    struct spg_device_channel overflow = {.name = "zz", .max = 1};
    if (spg_device_add_channel(&full, &overflow) != SPG_E_LIMIT) {
        return 1;
    }
    return 0;
}

/* The refusals, all reached with fd == -1. If any of these ever returns
 * SPG_E_INVALID_STATE instead of its own code, the check has drifted back
 * behind the connection test and stopped being testable. */
static int test_write_refusals(void) {
    struct spg_device dev = {};
    spg_device_init(&dev);

    struct spg_device_channel heater = {
        .name = "heater", .reg = 1u, .min = 0, .max = 100, .writable = true};
    struct spg_device_channel temp = {
        .name = "temp", .reg = 0u, .min = -400, .max = 9000};
    if (spg_device_add_channel(&dev, &heater) != SPG_OK ||
        spg_device_add_channel(&dev, &temp) != SPG_OK) {
        return 1;
    }

    if (spg_device_write(&dev, "nope", 1) != SPG_E_NOT_FOUND) {
        return 1;
    }
    if (spg_device_write(&dev, "temp", 100) != SPG_E_POLICY_DENIED) {
        return 1;
    }
    /* Out of range is refused, never clamped to 100. */
    if (spg_device_write(&dev, "heater", 101) != SPG_E_LIMIT ||
        spg_device_write(&dev, "heater", -1) != SPG_E_LIMIT) {
        return 1;
    }
    /* In range, so it gets past every refusal and only then finds no socket. */
    if (spg_device_write(&dev, "heater", 100) != SPG_E_INVALID_STATE) {
        return 1;
    }
    return 0;
}

static int test_codec(void) {
    uint8_t frame[SPG_MODBUS_FRAME_CAP] = {};

    if (spg_modbus_encode_read(0x1234u, 7u, 0x0102u, frame) != 12u) {
        return 1;
    }
    /* Big-endian on the wire whatever the host is. Spelled out byte by byte
     * because that is the whole property being checked. */
    const uint8_t expect_read[12] = {0x12, 0x34, 0x00, 0x00, 0x00, 0x06,
                                     0x07, 0x03, 0x01, 0x02, 0x00, 0x01};
    if (memcmp(frame, expect_read, sizeof expect_read) != 0) {
        return 1;
    }

    if (spg_modbus_encode_write(1u, 1u, 1u, 60u, frame) != 12u) {
        return 1;
    }
    const uint8_t expect_write[12] = {0x00, 0x01, 0x00, 0x00, 0x00, 0x06,
                                      0x01, 0x06, 0x00, 0x01, 0x00, 0x3c};
    if (memcmp(frame, expect_write, sizeof expect_write) != 0) {
        return 1;
    }

    uint16_t value = 0u;
    /* A read response carrying 235 — 23.5 C in tenths. */
    const uint8_t response[11] = {0x00, 0x05, 0x00, 0x00, 0x00, 0x05,
                                  0x01, 0x03, 0x02, 0x00, 0xeb};
    if (spg_modbus_decode(sizeof response, response, 5u, &value) != SPG_OK ||
        value != 235u) {
        return 1;
    }

    /* Somebody else's reply. Accepting it would attribute one machine's
     * reading to another on a gateway-fronted bus. */
    if (spg_modbus_decode(sizeof response, response, 6u, &value) !=
        SPG_E_REPLAY_MISMATCH) {
        return 1;
    }

    /* Exception response: function | 0x80. The device refused. */
    const uint8_t refusal[9] = {0x00, 0x05, 0x00, 0x00, 0x00,
                                0x03, 0x01, 0x83, 0x02};
    if (spg_modbus_decode(sizeof refusal, refusal, 5u, &value) != SPG_E_IO) {
        return 1;
    }

    /* Truncated frames must not be read past their end. */
    for (size_t n = 0u; n < sizeof response; n += 1u) {
        uint16_t ignored = 0u;
        if (spg_modbus_decode(n, response, 5u, &ignored) == SPG_OK) {
            return 1;
        }
    }
    return 0;
}

int main(void) {
    struct {
        const char *name;
        int (*fn)(void);
    } cases[] = {
        {"parse_channel", test_parse_channel},
        {"table", test_table},
        {"write_refusals", test_write_refusals},
        {"codec", test_codec},
    };
    for (size_t i = 0u; i < sizeof cases / sizeof cases[0]; i += 1u) {
        if (cases[i].fn() != 0) {
            (void)fprintf(stderr, "test_device: FAIL — %s\n", cases[i].name);
            return 1;
        }
    }
    (void)printf("test_device: PASS\n");
    return 0;
}
