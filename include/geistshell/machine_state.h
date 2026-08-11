#ifndef GEISTSHELL_MACHINE_STATE_H
#define GEISTSHELL_MACHINE_STATE_H

#include "geistshell/status.h"

#include <stdbool.h>
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

/* Kernel comm is TASK_COMM_LEN (16) including the NUL — names are truncated by
 * the kernel itself, which the profile matcher has to account for. */
#define SPG_PROCESS_NAME_CAP 16u

/* Hard ceiling on the processes carried in one snapshot. Selection decides
 * which ones survive; see spg_process_select. */
#define SPG_MACHINE_MAX_PROCESSES 64u

/* How many processes the sampler looks at before choosing. Larger than the
 * snapshot on purpose: a selection that only ever sees the first sixty-four
 * the kernel listed is not a selection. Found the hard way on a Pi with 167
 * processes, where kernel threads filled the context and the busiest process
 * was never considered. */
#define SPG_MACHINE_RAW_PROCESSES 1024u

/* profile_index of a process no profile entry matched. */
#define SPG_PROCESS_NO_PROFILE UINT32_MAX

/* Profile id length. Lives here, not in process_profile.h, because a sample
 * carries the id it matched — process_profile.h includes this header, so the
 * dependency can only point one way. */
#define SPG_PROCESS_ID_CAP 32u

/* Which parts of the snapshot to leave OUT of the rendered block (roadmap
 * phase 11, #71).
 *
 * A mask rather than a variant per experiment: six hand-written renderers would
 * drift apart, and the question is what the model needs, not how many ways
 * there are to print a struct. Mask 0 is the full block, byte-identical to
 * before this existed.
 *
 * Ablating a field REMOVES it. It does not set it to `unknown` — that would
 * measure how the model handles a broken sensor, which is a different
 * question. */
#define SPG_ABLATE_NONE 0u
#define SPG_ABLATE_ROLE (1u << 0)        /* process roles */
#define SPG_ABLATE_TEMPERATURE (1u << 1) /* temperature and throttle */
#define SPG_ABLATE_FREQUENCY (1u << 2)
#define SPG_ABLATE_MEMORY (1u << 3)    /* memory and swap */
#define SPG_ABLATE_PROCESSES (1u << 4) /* the process list entirely */
#define SPG_ABLATE_LOAD (1u << 5)      /* cpu load and load average */

/* Upper bound on a rendered (machine-state ...) block: the header fields plus
 * SPG_MACHINE_MAX_PROCESSES process entries. A caller can size a stack buffer
 * from this and never truncate. */
#define SPG_MACHINE_RENDER_CAP 8192u

/* Semantic role, supplied by the process profile — never guessed from the
 * name. Phase 6 turns may_pause/may_stop into policy decisions, so these are
 * typed fields and not prompt text. */
enum spg_process_role {
    SPG_PROCESS_ROLE_UNKNOWN = 0,
    SPG_PROCESS_ROLE_CRITICAL,
    SPG_PROCESS_ROLE_BATCH,
};

/* One process. No command line, no environment, no user name — those can carry
 * secrets and would be prompt-injection surface once this reaches the model. */
struct spg_process_sample {
    uint64_t pid;
    /* Kernel start time in clock ticks since boot. Together with pid this is
     * the process identity: a recycled pid gets a different start time, which
     * is what stops phase 6 from signalling the wrong process. */
    uint64_t start_identity;
    char     name[SPG_PROCESS_NAME_CAP];
    char     state; /* R, S, D, Z, T, ... as the kernel reports it */
    int64_t  nice;
    uint64_t cpu_time; /* raw utime+stime ticks, for the next delta */
    uint64_t cpu_bp;   /* share of one tick's total CPU, UNKNOWN without prev */
    uint64_t rss_bytes;

    /* Filled from the profile by spg_process_apply_profile. The id is copied
     * rather than looked up: it is what the context shows and what a phase-6
     * action targets, so the sample must be self-contained. */
    uint32_t              profile_index;
    char                  profile_id[SPG_PROCESS_ID_CAP];
    enum spg_process_role role;
    bool                  may_pause;
    bool                  may_stop;
};

#define SPG_PROCESS_MATCH_CAP 64u
#define SPG_PROCESS_PROFILE_CAP 16u

/* Which processes are managed, and what may be done to them. The type lives
 * here rather than in process_profile.h because the sampler must apply it
 * before selection ranks by role; the DSL that parses it stays in
 * process_profile.h. */
struct spg_process_profile_entry {
    char                  id[SPG_PROCESS_ID_CAP];
    char                  match[SPG_PROCESS_MATCH_CAP];
    enum spg_process_role role;
    bool                  may_pause;
    bool                  may_stop;
};

struct spg_process_profile {
    size_t                           count;
    struct spg_process_profile_entry entries[SPG_PROCESS_PROFILE_CAP];
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
    /* One-minute load average x100. The five- and fifteen-minute figures were
     * carried in every snapshot and every fixture for a year and rendered into
     * nothing — a struct of three where one is read is two fields of upkeep
     * for no reader. */
    uint64_t                 load_1_cbp;
    struct spg_memory_sample memory;
    int64_t                  temperature_mc; /* UNKNOWN_S when unavailable */
    uint64_t                 cpu_freq_khz;
    enum spg_throttle_state  throttle;
    uint64_t                 process_count;

    struct spg_cpu_sample cpu; /* raw counters, so the caller can feed the
                                * next call without re-reading /proc/stat */

    /* Bounded and already selected: the snapshot never carries every process
     * on the host. n_processes <= SPG_MACHINE_MAX_PROCESSES, and
     * processes_truncated says whether anything was dropped — a consumer must
     * not read a short list as "this is all that runs". */
    size_t                    n_processes;
    bool                      processes_truncated;
    struct spg_process_sample processes[SPG_MACHINE_MAX_PROCESSES];
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

/* One-minute load average x100 from "0.42 0.31 0.28 1/234 5678". */
[[nodiscard]] enum spg_status
spg_telemetry_parse_loadavg(size_t n, const char buf[], uint64_t *out_1_cbp);

/* Single-integer sysfs files (thermal zone temp, scaling_cur_freq). Accepts a
 * trailing newline and a leading '-'. */
[[nodiscard]] enum spg_status
spg_telemetry_parse_int(size_t n, const char buf[], int64_t *out);

/* Pi firmware bitmask, e.g. "0x50005" from get_throttled. */
[[nodiscard]] enum spg_throttle_state
spg_telemetry_parse_throttle(size_t n, const char buf[]);

/* One /proc/<pid>/stat line. page_bytes converts the RSS page count the kernel
 * reports; the caller supplies it so this stays a pure function.
 *
 * The comm field is the reason this is not a naive field split: the kernel
 * writes it in parentheses without escaping, so it can contain spaces and
 * ')' — "(my app) (weird)" is a legal name. Fields are read after the LAST
 * ')' in the line. */
[[nodiscard]] enum spg_status
spg_process_parse_stat(size_t n, const char buf[], uint64_t page_bytes,
                       struct spg_process_sample *out);

/* Share of one tick's total CPU time, in basis points. total_delta is the
 * delta of spg_cpu_sample.total across the same interval. Returns UNKNOWN
 * when prev is null (first sighting), when the identity does not match (pid
 * reuse), when nothing advanced, or when a counter went backwards. */
[[nodiscard]] uint64_t
spg_process_utilisation_bp(const struct spg_process_sample *prev,
                           const struct spg_process_sample *cur,
                           uint64_t                         total_delta);

/* Find a process by identity — pid AND start_identity, never pid alone.
 * Returns null when absent. */
[[nodiscard]] const struct spg_process_sample *
spg_process_find(size_t n, const struct spg_process_sample procs[],
                 uint64_t pid, uint64_t start_identity);

/* Offer one candidate to a running best-of buffer, replacing the weakest entry
 * once it is full. Returns whether the candidate was kept.
 *
 * This exists because the enumerator cannot hold every process on a real host —
 * a Pi 5 idles at ~170. Without it, selection only ever saw the first `cap`
 * processes /proc happened to list, so the busiest process could be invisible
 * while kernel threads at 0% filled the snapshot. Ranking is the same as
 * spg_process_select's. */
[[nodiscard]] bool
spg_process_offer(size_t cap, struct spg_process_sample buf[], size_t *n,
                  const struct spg_process_sample *candidate);

/* Pick which processes reach the snapshot, in a fixed order:
 *   1. profile-managed processes  2. CPU descending
 *   3. RSS descending             4. pid ascending
 * The last rung makes the result independent of the order the OS enumerated
 * them in — /proc has no ordering guarantee, and an unstable context would
 * break byte-identical replay. Sets *out_truncated when anything was dropped.
 * in[] rather than in[static n_in]: an empty process list is legitimate. */
[[nodiscard]] enum spg_status
spg_process_select(size_t n_in, const struct spg_process_sample in[],
                   size_t out_cap, struct spg_process_sample out[],
                   size_t *out_n, bool *out_truncated);

[[nodiscard]] const char *
spg_process_role_to_string(enum spg_process_role role);

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

/* Same, plus the previous tick's process list so per-process CPU can be a
 * delta. Processes are matched by pid AND start identity: a recycled pid does
 * not inherit the old process's counters, it reports unknown. prev_procs may
 * be null when prev_n is 0. */
/* profile may be null (nothing managed). It is applied per sample, before
 * selection: selection ranks managed processes first, so applying it later
 * would make that rule a no-op — and a managed process that currently costs
 * nothing must still reach the snapshot. */
[[nodiscard]] enum spg_status spg_machine_sample_with_processes(
    uint64_t timestamp_ns, const struct spg_cpu_sample *prev, size_t prev_n,
    const struct spg_process_sample   prev_procs[],
    const struct spg_process_profile *profile, struct spg_machine_state *out);

/* --- layer 3: serialisation --------------------------------------------- */

/* Deterministic s-expression: fixed field order, unknown values as the symbol
 * `unknown`. Identical input always yields identical bytes. Writes at most
 * dst_capacity bytes including the NUL; on SPG_E_LIMIT *out_required holds the
 * size needed and dst holds no partial record.
 *
 * Processes render in snapshot order (already ranked by spg_process_select) as
 *   (process (id "batch_job") (role batch) (cpu-bp 3100) (rss-bytes 1024))
 * where id is the profile id for a managed process and the process name
 * otherwise — one shape, because two would cost a small model accuracy. The
 * pid is deliberately absent: the model never needs it, and a phase-6 action
 * targets the profile id while the executor re-validates identity itself.
 *
 * When processes were dropped, a trailing (processes-dropped N) says so. A
 * short list that looks complete would invite wrong conclusions. */
[[nodiscard]] enum spg_status
spg_machine_state_render(const struct spg_machine_state *state,
                         size_t dst_capacity, char dst[static dst_capacity],
                         size_t *out_required);

/* Same, with parts left out. spg_machine_state_render is this with mask 0 —
 * one implementation, so an ablated block cannot drift from the full one. */
[[nodiscard]] enum spg_status spg_machine_state_render_masked(
    const struct spg_machine_state *state, uint32_t ablate, size_t dst_capacity,
    char dst[static dst_capacity], size_t *out_required);

[[nodiscard]] const char *
spg_throttle_state_to_string(enum spg_throttle_state state);

#ifdef __cplusplus
}
#endif

#endif
