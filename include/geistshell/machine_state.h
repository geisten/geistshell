#ifndef GEISTSHELL_MACHINE_STATE_H
#define GEISTSHELL_MACHINE_STATE_H

#include "geistshell/status.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read-only machine telemetry (roadmap phase 1). Three layers, kept apart on
 * purpose:
 *   1. OS reading      — spg_machine_sample(), platform-specific, does I/O
 *   2. normalisation   — the parsers below, pure functions over buffers
 *   3. serialisation   — spg_machine_state_render(), deterministic s-expression
 *
 * The module never reads a clock. Like the journal (spg_journal_writer_append),
 * it takes timestamp_ns from the caller, so replay stays byte-identical and
 * tests need no time mocking. See docs/machine-intelligence/Baseline.md.
 *
 * All values are integers in fixed-point units — never floats, whose formatting
 * depends on the locale and whose rounding would break byte-identical output.
 * Unit suffixes: _bp = basis points (1/10000), _cbp = centi (1/100),
 * _mc = millidegrees Celsius, _khz, _bytes.
 */

/* Optional values are explicit, never 0. A field that could not be read holds
 * the sentinel; every consumer must test for it before using the value. */
#define SPG_MACHINE_UNKNOWN UINT64_MAX
#define SPG_MACHINE_UNKNOWN_S INT64_MIN

/* Raw CPU time counters from /proc/stat. Meaningless alone: utilisation is the
 * delta between two samples, which is why the caller keeps the previous one. */
struct spg_cpu_sample {
    uint64_t idle;  /* idle + iowait */
    uint64_t total; /* all fields summed, including idle */
};

struct spg_memory_sample {
    uint64_t total_bytes;
    uint64_t used_bytes; /* total - available; available, not free */
    uint64_t swap_used_bytes;
};

struct spg_load_sample {
    uint64_t avg_1_cbp; /* load average x100, so 1.75 -> 175 */
    uint64_t avg_5_cbp;
    uint64_t avg_15_cbp;
};

/* Raspberry Pi exposes a throttling bitmask; most hosts expose nothing. */
enum spg_throttle_state {
    SPG_THROTTLE_UNKNOWN = 0,
    SPG_THROTTLE_NONE,
    SPG_THROTTLE_ACTIVE, /* currently throttled or under-voltage */
    SPG_THROTTLE_PAST,   /* not now, but it happened since boot */
};

/* One normalised snapshot. Fixed size, no pointers, no allocation: it can be
 * copied into a ring buffer (phase 3b) without ownership questions. */
struct spg_machine_state {
    uint64_t timestamp_ns; /* injected by the caller, never read from a clock */

    uint64_t cpu_utilisation_bp; /* 0..10000, UNKNOWN on the first sample */
    struct spg_load_sample   load;
    struct spg_memory_sample memory;
    int64_t                  temperature_mc; /* UNKNOWN_S when unavailable */
    uint64_t                 cpu_freq_khz;
    enum spg_throttle_state  throttle;
    uint64_t                 process_count;

    struct spg_cpu_sample cpu; /* raw counters, so the caller can feed the
                                * next call without re-reading /proc/stat */
};

/* --- layer 2: parsers, pure, no I/O ------------------------------------- */
/* Each takes the buffer length before the buffer and reads at most n bytes;
 * none requires NUL termination. Malformed input yields SPG_E_FORMAT and
 * leaves *out unknown/zeroed; counter overflow yields SPG_E_OVERFLOW.
 *
 * The parameter is `buf[]`, not `buf[static n]`: an empty file is a legitimate
 * input here (a sysfs file can exist and read back zero bytes), and `[static
 * 0]` is undefined behaviour. Same reasoning as spg_cmd_executor_run's empty
 * batch, see cmd_executor.h. */

/* First "cpu " line of /proc/stat. */
[[nodiscard]] enum spg_status
spg_telemetry_parse_stat(size_t n, const char buf[],
                         struct spg_cpu_sample *out);

/* MemTotal/MemAvailable/SwapTotal/SwapFree of /proc/meminfo (kB units). */
[[nodiscard]] enum spg_status
spg_telemetry_parse_meminfo(size_t n, const char buf[],
                            struct spg_memory_sample *out);

/* "0.42 0.31 0.28 1/234 5678" of /proc/loadavg. */
[[nodiscard]] enum spg_status
spg_telemetry_parse_loadavg(size_t n, const char buf[],
                            struct spg_load_sample *out);

/* Single-integer sysfs files (thermal zone temp, scaling_cur_freq). Accepts a
 * trailing newline and a leading '-'. */
[[nodiscard]] enum spg_status
spg_telemetry_parse_int(size_t n, const char buf[], int64_t *out);

/* Pi firmware bitmask, e.g. "0x50005" from get_throttled. */
[[nodiscard]] enum spg_throttle_state
spg_telemetry_parse_throttle(size_t n, const char buf[]);

/* Utilisation between two samples. Returns SPG_MACHINE_UNKNOWN when prev is
 * null, the counters did not advance, or they went backwards (a counter reset
 * across suspend/resume) — never a wrong number and never a division by zero.
 */
[[nodiscard]] uint64_t
spg_telemetry_utilisation_bp(const struct spg_cpu_sample *prev,
                             const struct spg_cpu_sample *cur);

/* --- layer 1: OS reading ------------------------------------------------ */

/* Fill out from the host. prev may be null (first tick: utilisation stays
 * UNKNOWN). Returns SPG_E_UNSUPPORTED on non-Linux hosts with every field set
 * to unknown — a caller must be able to keep running on such a host.
 * Individual missing files are not an error; they leave their field unknown. */
[[nodiscard]] enum spg_status
spg_machine_sample(uint64_t timestamp_ns, const struct spg_cpu_sample *prev,
                   struct spg_machine_state *out);

/* --- layer 3: serialisation --------------------------------------------- */

/* Deterministic s-expression: fixed field order, unknown values as the symbol
 * `unknown`. Identical input always yields identical bytes. Writes at most
 * dst_capacity bytes including the NUL; on SPG_E_LIMIT *out_required holds the
 * size needed and dst holds no partial record. */
[[nodiscard]] enum spg_status
spg_machine_state_render(const struct spg_machine_state *state,
                         size_t dst_capacity, char dst[static dst_capacity],
                         size_t *out_required);

[[nodiscard]] const char *
spg_throttle_state_to_string(enum spg_throttle_state state);

#ifdef __cplusplus
}
#endif

#endif
