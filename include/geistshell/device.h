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
 * The transport is ONE thing: a program.
 *
 *     read:   execve(program)            -> an integer on stdout, exit 0
 *     write:  execve(program, "<value>") -> exit 0 means accepted
 *
 * A sensor is a program that prints a number; an actuator is a program that
 * takes one. Any device is attachable — I2C, MQTT, HTTP, a cat on sysfs, a
 * three-line Python script — without this repo ever being touched again.
 * Modbus TCP lived here once and won what it had to win: the same code path
 * drove simulated and real machines. It left without a successor program;
 * decades-old tools (mbpoll and friends) already speak it from a shell line.
 *
 * No shell sits between geistshell and the program: execve directly, the
 * value as its own argv element. A model-chosen setpoint is never part of a
 * command line an interpreter reads, so there is no quoting anyone can forget.
 *
 * Values are int64_t in raw register units. Devices scale their own registers
 * (23.5 °C is commonly the integer 235), and a scale factor per channel would
 * be a second place for a wrong number to live — and floats would break the
 * byte-identical replay. */

constexpr size_t SPG_DEVICE_NAME_CAP     = 32u;
constexpr size_t SPG_DEVICE_PROGRAM_CAP  = 256u;
constexpr size_t SPG_DEVICE_MAX_CHANNELS = 32u;

/* Milliseconds before a silent program is killed (whole process group). A
 * device that does not answer must never be able to stall the loop that
 * governs it — an agent blocked on a read is an agent that cannot react to
 * anything else. */
constexpr int SPG_DEVICE_TIMEOUT_MS = 1000;

struct spg_device_channel {
    char    name[SPG_DEVICE_NAME_CAP];
    char    program[SPG_DEVICE_PROGRAM_CAP]; /* executed, never interpreted */
    int64_t min; /* inclusive, in register units */
    int64_t max; /* inclusive */
    bool writable; /* a read-only channel is refused, not silently ignored */
    /* Where this channel goes when contact is lost. Mandatory for a writable
     * channel and validated against the range: a writable channel with no
     * declared safe value is a channel nobody has decided about, and the
     * decision would then be made by whatever the machine was last told. */
    int64_t safe;
    /* Per-channel watchdog bookkeeping (#118). A successful transaction on
     * THIS channel is what feeds it — a sensor that answers must not keep an
     * actuator that has gone silent looking alive. Same injected clock unit
     * as the device-level fields. */
    uint64_t last_contact;
    bool     contact_pending;
};

struct spg_device {
    /* Watchdog. Never read from a clock in here — every timestamp in this
     * codebase is injected so a replay is byte-identical, and a watchdog is
     * the last place that should keep a private clock.
     *
     * The UNIT is whatever the caller's injected clock uses, because there is
     * no single answer: the CLI passes nanoseconds, and the agent loop's clock
     * is a step counter (`step + 1`), which is what makes its replay
     * deterministic. Judging a millisecond deadline against a step counter
     * would be a number that looks like time and is not. */
    uint64_t watchdog_timeout; /* 0 disables it */
    uint64_t last_contact;
    bool     contact_pending; /* a transaction succeeded since the last check */
    /* #118: the tick-level watchdog service latches after driving the safe
     * state so an expiry that persists does not re-drive (and re-journal) it
     * every tick; any successful contact re-arms the latch. */
    bool   watchdog_tripped;
    size_t n_channels;
    struct spg_device_channel channels[SPG_DEVICE_MAX_CHANNELS];
};

/* --- The table -------------------------------------------------------- */

void spg_device_init(struct spg_device *dev);

[[nodiscard]] enum spg_status
spg_device_add_channel(struct spg_device               *dev,
                       const struct spg_device_channel *channel);

/* Parse one channel form — the same language as policy and simulator, loaded
 * through sexpr.c, so there is no second parser and no second attack surface:
 *
 *     (channel (name "temp")   (program "/opt/plant/temp")   (range -400 9000))
 *     (channel (name "heater") (program "/opt/plant/heater") (range 0 100)
 *              (safe 0))
 *
 * (safe ...) PRESENT means writable. A separate writable flag would be a
 * second source for the same statement, and the rule "a writable channel must
 * declare a safe value" becomes structurally unbreakable instead of checked
 * after the fact. The named price: a (safe ...) on a sensor makes it quietly
 * writable — the range still bounds it, and a sensor with a safe value is a
 * typo a review sees. */
[[nodiscard]] enum spg_status
spg_device_parse_channel(size_t text_n, const char text[],
                         struct spg_device_channel *out);

/* Load a whole table:
 *
 *     (device
 *       (channel ...)
 *       (channel ...))
 *
 * Every channel is validated exactly as in spg_device_add_channel; the first
 * refusal aborts the load and reports which form failed via *out_bad (0-based
 * channel index, untouched on success). */
[[nodiscard]] enum spg_status spg_device_load(size_t text_n, const char text[],
                                              struct spg_device *dev,
                                              size_t            *out_bad);

[[nodiscard]] const struct spg_device_channel *
spg_device_find(const struct spg_device *dev, const char *name);

/* --- What the agent sees ----------------------------------------------- */

/* One tick's readings, one entry per channel. Fixed size, no pointers: it is
 * rendered into the context, so it must not own anything — same discipline as
 * struct spg_machine_state.
 *
 * The value is in register units, the same number the operator wrote the range
 * in. A reading that could not be taken is `known == false` and renders as
 * `unknown`, NEVER as 0. A dead sensor that looks like a zero measurement is
 * the most dangerous failure a controller can have: it is indistinguishable
 * from a real reading, and the next decision is built on it. */
struct spg_device_reading {
    char    name[SPG_DEVICE_NAME_CAP];
    int64_t value;
    bool    known;
};

struct spg_device_state {
    size_t                    n;
    struct spg_device_reading readings[SPG_DEVICE_MAX_CHANNELS];
};

/* Read every channel into `out`, in table order.
 *
 * A channel that fails is recorded unknown and does NOT abort the others — one
 * unreachable sensor must not blind the agent to the rest of the plant. Same
 * reasoning as spg_device_safe_state, which also attempts every channel.
 * Returns the FIRST failure, or SPG_OK when every channel answered; `out` is
 * complete either way.
 *
 * A successful read counts as contact, so a run that only observes keeps the
 * watchdog alive. Contact is contact — before this existed only a write fed
 * it, which made a purely observing run look like a machine that had gone
 * silent. */
[[nodiscard]] enum spg_status spg_device_sample(struct spg_device       *dev,
                                                struct spg_device_state *out);

/* Upper bound on a rendered block: 32 channels of a 32-byte name plus a signed
 * value and its parentheses. A caller can size a stack buffer from this and
 * never truncate. */
constexpr size_t SPG_DEVICE_RENDER_CAP = 2048u;

/* Deterministic s-expression, table order, unknown values as the symbol
 * `unknown`:
 *
 *     (device-state (temp 2350) (heater 40) (druck unknown))
 *
 * Identical input always yields identical bytes — the block travels into the
 * journal as part of the model input, so a replay depends on it. Writes at
 * most dst_capacity bytes including the NUL; on SPG_E_LIMIT *out_required
 * holds the size needed and dst holds no partial record.
 *
 * No units, no ranges, no scale. The channel name carries the meaning and the
 * table carries the bounds; a second copy in the prompt would be a second
 * place for a wrong number to live.
 * ponytail: the writable channels' ranges are deliberately NOT shown. The
 * setpoint is the one number the model free-decodes, so it can propose an
 * out-of-range value and burn a step. Add a range line when the plant eval
 * measures that rejection rate — not before. */
[[nodiscard]] enum spg_status
spg_device_state_render(const struct spg_device_state *state,
                        size_t dst_capacity, char dst[static dst_capacity],
                        size_t *out_required);

/* --- The watchdog ----------------------------------------------------- */

enum spg_device_watchdog {
    SPG_WATCHDOG_DISABLED = 0,
    SPG_WATCHDOG_OK,
    SPG_WATCHDOG_EXPIRED,
};

/* Arm the watchdog. A `timeout` of 0 disables it; `now` starts the clock, so
 * an armed watchdog does not fire on a machine that has simply not been spoken
 * to yet. Both are in the caller's clock unit — see the struct field. */
void spg_device_arm_watchdog(struct spg_device *dev, uint64_t timeout,
                             uint64_t now);

/* Fold in any successful transaction since the last call and report whether
 * the deadline has passed. Checking is what consumes the contact flags, so a
 * caller cannot succeed at keeping the machine alive while forgetting to
 * check — the two are the same call.
 *
 * EXPIRED is sticky until the next successful transaction: a machine that
 * answers once and goes quiet again must not look healthy in between.
 *
 * #118: the deadline is judged PER WRITABLE CHANNEL as well as globally. A
 * writable channel is fed only by a successful write to it (arming stamps a
 * baseline), so a sensor that keeps answering cannot mask an actuator whose
 * writes stopped landing — EXPIRED as soon as ANY writable channel has gone
 * a full timeout without contact. Read-only channels ride on the global
 * deadline as before: for a plant of sensors, reads are the only contact
 * there is. */
[[nodiscard]] enum spg_device_watchdog
spg_device_watchdog_check(struct spg_device *dev, uint64_t now);

/* Drive every writable channel to its declared safe value.
 *
 * Every channel is attempted even after one fails, and the FIRST failure is
 * returned. A machine stopped half-way to safe is worse than either end, so
 * one unreachable channel must never abort the ones that would still obey. */
[[nodiscard]] enum spg_status spg_device_safe_state(struct spg_device *dev);

/* --- The machine ------------------------------------------------------ */

/* Run the channel's program with no argument and parse one integer from its
 * stdout. Exit != 0, unparsable output or the timeout are SPG_E_IO — and the
 * reading stays untouched, never 0. There is no connect step: the first read
 * IS the connectivity probe, and a plant that cannot be read is something the
 * caller is told about per channel, not a session that failed to open. */
[[nodiscard]] enum spg_status spg_device_read(struct spg_device *dev,
                                              const char *name, int64_t *out);

/* Run the channel's program with the value as its single argument.
 *
 * Out-of-range is REFUSED (SPG_E_LIMIT), not clamped. Clamping executes a
 * near-miss of a command that was already wrong, which is how machines get
 * damaged by software that believed it was being helpful. The caller learns
 * its value was rejected; nothing is executed.
 *
 * Every refusal — unknown channel, read-only, out of range — is decided
 * BEFORE the fork, so all of it is testable without a program ever starting.
 * A safety check reachable only once a machine is plugged in is a safety
 * check nobody runs.
 *
 * Echo is possible but voluntary: a write program that prints a number gets
 * it compared against the commanded value (mismatch is SPG_E_IO — a device
 * that acknowledged a different value than it was told is a device the next
 * decision must not trust); a silent one is taken at its exit code. */
[[nodiscard]] enum spg_status spg_device_write(struct spg_device *dev,
                                               const char *name, int64_t value);

#ifdef __cplusplus
}
#endif

#endif
