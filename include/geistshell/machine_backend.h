#ifndef GEISTSHELL_MACHINE_BACKEND_H
#define GEISTSHELL_MACHINE_BACKEND_H

#include "geistshell/machine_state.h"
#include "geistshell/status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The whole surface geistshell needs from an operating system.
 *
 * Everything above this line is portable C: the parsers, the selection, the
 * renderer, the policy, the ledger. Everything below it is one file per
 * platform, chosen at link time by the Makefile — no function pointers, no
 * macro dispatch, and no `#if defined(__linux__)` scattered through logic that
 * has nothing to do with an operating system.
 *
 * That split was not free of charge to learn. The fan clamp in thermal.c sat
 * inside a platform branch until a test caught it: a safety property that
 * silently did not exist off Linux and could not be tested on the machine it
 * was written on. A port boundary is worth having precisely because it stops
 * decisions from hiding inside I/O.
 *
 * Every function here follows one rule: **a value that cannot be read is
 * unknown, never zero, and never an error that stops the run.** A host without
 * a temperature sensor is a host, not a failure.
 *
 * Nothing in this interface is CPU-architecture specific. Counters are
 * normalised to ratios by the callers, so tick rates, page sizes and word
 * widths stay inside the backend that knows them. */

/* Which backend was linked. Goes into the journal and the benchmark records:
 * a measurement without the platform that produced it cannot be compared. */
[[nodiscard]] const char *spg_backend_name(void);

/* True when this backend reads a real machine. The generic fallback returns
 * false, which lets a caller distinguish "the sensor is missing" from "this
 * platform was never ported". */
[[nodiscard]] bool spg_backend_is_live(void);

/* Aggregate CPU time counters. Units are the platform's own — only deltas
 * between two samples are ever used, so the tick rate never leaves here.
 * Returns SPG_E_UNSUPPORTED when the platform cannot report them.
 *
 * ONE CONTRACT BINDS A BACKEND: this counter and spg_process_sample.cpu_time
 * MUST share a unit. Per-process utilisation is the ratio of the two, and a
 * ratio between microseconds and ticks is not a small error — it pins every
 * process at 100%. The macOS port did exactly that on its first run, which is
 * why the rule is written down here rather than assumed. */
[[nodiscard]] enum spg_status spg_backend_cpu(struct spg_cpu_sample *out);

/* Total and used memory in bytes. "Used" means what a workload cannot have:
 * total minus what the kernel would hand out on demand. */
[[nodiscard]] enum spg_status spg_backend_memory(struct spg_memory_sample *out);

/* Load averages, x100. */
[[nodiscard]] enum spg_status spg_backend_load(struct spg_load_sample *out);

/* Millidegrees Celsius. SPG_E_UNSUPPORTED where no public interface exists —
 * macOS is such a platform, and pretending otherwise would mean shipping a
 * private-API dependency for a number the roadmap already treats as optional.
 */
[[nodiscard]] enum spg_status spg_backend_temperature(int64_t *out_mc);

[[nodiscard]] enum spg_status spg_backend_frequency_khz(uint64_t *out_khz);

[[nodiscard]] enum spg_status
spg_backend_throttle(enum spg_throttle_state *out);

/* Fill up to `cap` process samples and report how many exist in total, which
 * is not the same number: the snapshot is deliberately smaller than the
 * machine. Each sample carries pid, start identity, name, state, nice, raw CPU
 * time and RSS; cpu_bp is left unknown because a rate needs two samples.
 *
 * `total` may exceed `*out_n` — a caller uses it to report truncation. */
[[nodiscard]] enum spg_status
spg_backend_processes(size_t cap, struct spg_process_sample out[],
                      size_t *out_n, uint64_t *out_total);

/* The start identity of one live process, for the re-validation phase 6 does
 * immediately before signalling. Returns SPG_E_NOT_FOUND when the pid is gone
 * — which is the answer that matters, because a pid that no longer exists must
 * never be signalled.
 *
 * Separate from spg_backend_processes on purpose: the executor asks about ONE
 * pid at the last possible moment, and walking the whole process table to
 * answer that would widen the window it exists to close. */
[[nodiscard]] enum spg_status
spg_backend_process_identity(uint64_t pid, uint64_t *out_start_identity);

/* --- machine B ---------------------------------------------------------- */

/* Fan speed and duty. SPG_E_UNSUPPORTED on hosts with no controllable fan,
 * which is most of them. */
[[nodiscard]] enum spg_status spg_backend_fan_read(uint64_t *out_rpm,
                                                   uint64_t *out_duty);

/* Write the fan duty. The clamp is applied by the caller — it is a safety
 * decision and belongs above the port boundary, not inside it. */
[[nodiscard]] enum spg_status spg_backend_fan_write(uint64_t duty);

#ifdef __cplusplus
}
#endif

#endif
