#ifndef GEISTSHELL_DEVICE_H
#define GEISTSHELL_DEVICE_H

#include "geistshell/status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reading and writing a real machine.
 *
 * A CHANNEL is the entire vocabulary: a name, a range, and whether it may be
 * written. Not one function per peripheral — that shape is what cost us the
 * fan port, where three backends each carried two functions for a device no
 * executor ever drove. A new machine is a new table, not new code.
 *
 * The transport is Modbus TCP because it is what industrial machines actually
 * speak, and because the simulators speak the same thing. The property that
 * matters is the one gpio-sim has a layer lower: the code driving a simulated
 * machine and the code driving a real one are the same code, so there is no
 * simulation branch that can rot while nobody looks at it.
 *
 * Values are int64_t in raw register units. Devices scale their own registers
 * (23.5 °C is commonly the integer 235), and a scale factor per channel would
 * be a second place for a wrong number to live.
 * ponytail: no scaling, no 32-bit or float registers. Add a scale to the
 * channel when a device needs one, not before. */

constexpr size_t SPG_DEVICE_NAME_CAP     = 32u;
constexpr size_t SPG_DEVICE_MAX_CHANNELS = 32u;

/* Milliseconds before a silent machine is given up on. A device that does not
 * answer must never be able to stall the loop that governs it — an agent
 * blocked in read() is an agent that cannot react to anything else. */
constexpr int SPG_DEVICE_TIMEOUT_MS = 1000;

struct spg_device_channel {
    char     name[SPG_DEVICE_NAME_CAP];
    uint16_t unit; /* Modbus unit id, for gateways fronting several nodes */
    uint16_t reg;  /* holding-register address */
    int64_t  min;  /* inclusive, in register units */
    int64_t  max;  /* inclusive */
    bool writable; /* a read-only channel is refused, not silently ignored */
};

struct spg_device {
    int      fd;  /* -1 when not connected */
    uint16_t txn; /* Modbus transaction id, incremented per request */
    size_t   n_channels;
    struct spg_device_channel channels[SPG_DEVICE_MAX_CHANNELS];
};

/* --- The table -------------------------------------------------------- */

void spg_device_init(struct spg_device *dev);

[[nodiscard]] enum spg_status
spg_device_add_channel(struct spg_device               *dev,
                       const struct spg_device_channel *channel);

/* Parse one "name:reg:min:max:rw" descriptor. Deliberately not a config file
 * format of its own: there is exactly one machine to describe so far, and a
 * parser is a thing that has to be maintained whether or not anyone uses it.
 * ponytail: promote to the sexpr config when a second machine shows up. */
[[nodiscard]] enum spg_status
spg_device_parse_channel(size_t text_n, const char text[],
                         struct spg_device_channel *out);

[[nodiscard]] const struct spg_device_channel *
spg_device_find(const struct spg_device *dev, const char *name);

/* --- The wire --------------------------------------------------------- */

/* Frame codecs, kept free of any socket so the wire format can be tested
 * without a network. Both write a complete MBAP header plus PDU and return
 * the byte count. */
constexpr size_t SPG_MODBUS_FRAME_CAP = 12u;

size_t spg_modbus_encode_read(uint16_t txn, uint16_t unit, uint16_t reg,
                              uint8_t out[static SPG_MODBUS_FRAME_CAP]);
size_t spg_modbus_encode_write(uint16_t txn, uint16_t unit, uint16_t reg,
                               uint16_t value,
                               uint8_t  out[static SPG_MODBUS_FRAME_CAP]);

/* Decode a response and check it answers `txn`. A mismatched transaction id is
 * SPG_E_REPLAY_MISMATCH, not a warning: on a shared bus it means this reply
 * belongs to somebody else's question, and using it would attribute one
 * machine's reading to another.
 *
 * A Modbus exception response becomes SPG_E_IO; out_value is untouched. */
[[nodiscard]] enum spg_status spg_modbus_decode(size_t n, const uint8_t frame[],
                                                uint16_t  txn,
                                                uint16_t *out_value);

/* --- The machine ------------------------------------------------------ */

[[nodiscard]] enum spg_status
spg_device_connect(struct spg_device *dev, const char *host, uint16_t port);

void spg_device_close(struct spg_device *dev);

[[nodiscard]] enum spg_status spg_device_read(struct spg_device *dev,
                                              const char *name, int64_t *out);

/* Out-of-range is REFUSED (SPG_E_LIMIT), not clamped. Clamping executes a
 * near-miss of a command that was already wrong, which is how machines get
 * damaged by software that believed it was being helpful. The caller learns
 * its value was rejected; nothing is written.
 *
 * The check lives here, above the socket, for the same reason the fan clamp
 * had to leave its platform branch: a safety property inside an I/O path is a
 * safety property that cannot be tested. */
[[nodiscard]] enum spg_status spg_device_write(struct spg_device *dev,
                                               const char *name, int64_t value);

#ifdef __cplusplus
}
#endif

#endif
