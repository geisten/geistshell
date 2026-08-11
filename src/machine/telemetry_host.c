/* Layer 1: assemble a snapshot from whatever the backend can read.
 *
 * This file no longer knows what an operating system is. It decides WHICH
 * questions to ask and what to do with a missing answer; the backend knows how
 * to ask them. That boundary is why the same sampler serves Linux, macOS and a
 * host nobody has ported. */

#include "geistshell/machine_backend.h"
#include "geistshell/machine_state.h"
#include "geistshell/process_profile.h"

static void state_init(struct spg_machine_state *out,
                       const uint64_t            timestamp_ns) {
    *out = (struct spg_machine_state){
        .timestamp_ns       = timestamp_ns,
        .cpu_utilisation_bp = SPG_MACHINE_UNKNOWN,
        .load =
            {
                .avg_1_cbp  = SPG_MACHINE_UNKNOWN,
                .avg_5_cbp  = SPG_MACHINE_UNKNOWN,
                .avg_15_cbp = SPG_MACHINE_UNKNOWN,
            },
        .memory =
            {
                .total_bytes     = SPG_MACHINE_UNKNOWN,
                .used_bytes      = SPG_MACHINE_UNKNOWN,
                .swap_used_bytes = SPG_MACHINE_UNKNOWN,
            },
        .temperature_mc = SPG_MACHINE_UNKNOWN_S,
        .cpu_freq_khz   = SPG_MACHINE_UNKNOWN,
        .throttle       = SPG_THROTTLE_UNKNOWN,
        .process_count  = SPG_MACHINE_UNKNOWN,
    };
}

/* Rank, filter and rate the processes the backend produced.
 *
 * All three steps are portable: the backend hands over raw samples, and what
 * counts as interesting is a decision, not an operating-system detail. */
static void select_processes(
    const size_t raw_n, struct spg_process_sample raw[], const size_t prev_n,
    const struct spg_process_sample prev[], const uint64_t total_delta,
    const struct spg_process_profile *profile, struct spg_machine_state *out) {
    struct spg_process_sample kept[SPG_MACHINE_MAX_PROCESSES];
    size_t                    n_kept = 0u;

    for (size_t i = 0u; i < raw_n; i += 1u) {
        struct spg_process_sample *sample = &raw[i];
        /* Roles first: both the filter below and the ranking depend on knowing
         * what is managed. */
        spg_process_apply_profile(profile, 1u, sample);

        const struct spg_process_sample *before =
            spg_process_find(prev_n, prev, sample->pid, sample->start_identity);
        sample->cpu_bp =
            spg_process_utilisation_bp(before, sample, total_delta);

        /* A process with no memory and no measurable CPU carries no signal. On
         * a Pi 5 that is ~100 kernel threads, which filled 58 of 64 context
         * slots with "(rss-bytes 0)" during the hardware test. Managed
         * processes are always kept: a paused batch job that currently costs
         * nothing is exactly what the model must still see. */
        const bool has_memory =
            sample->rss_bytes != 0u && sample->rss_bytes != SPG_MACHINE_UNKNOWN;
        const bool has_cpu =
            sample->cpu_bp != SPG_MACHINE_UNKNOWN && sample->cpu_bp != 0u;
        if (!has_memory && !has_cpu &&
            sample->profile_index == SPG_PROCESS_NO_PROFILE) {
            continue;
        }
        (void)spg_process_offer(SPG_MACHINE_MAX_PROCESSES, kept, &n_kept,
                                sample);
    }

    (void)spg_process_select(n_kept, kept, SPG_MACHINE_MAX_PROCESSES,
                             out->processes, &out->n_processes,
                             &out->processes_truncated);
}

enum spg_status spg_machine_sample_with_processes(
    const uint64_t timestamp_ns, const struct spg_cpu_sample *prev,
    const size_t prev_n, const struct spg_process_sample prev_procs[],
    const struct spg_process_profile *profile, struct spg_machine_state *out) {
    if (out == nullptr || (prev_n > 0u && prev_procs == nullptr)) {
        return SPG_E_INVALID_ARG;
    }
    state_init(out, timestamp_ns);
    if (!spg_backend_is_live()) {
        /* Distinct from a host whose sensors are merely missing: nothing was
         * ported here, and a caller that reports platform coverage needs to be
         * able to tell those apart. */
        return SPG_E_UNSUPPORTED;
    }

    uint64_t total_delta = 0u;
    if (spg_backend_cpu(&out->cpu) == SPG_OK) {
        out->cpu_utilisation_bp = spg_telemetry_utilisation_bp(prev, &out->cpu);
        if (prev != nullptr && out->cpu.total >= prev->total) {
            total_delta = out->cpu.total - prev->total;
        }
    }
    /* Each of these leaves its field unknown on failure, which state_init
     * already arranged. A missing sensor is never an error here — that is the
     * difference between a snapshot and an assertion. */
    (void)spg_backend_memory(&out->memory);
    (void)spg_backend_load(&out->load);
    (void)spg_backend_temperature(&out->temperature_mc);
    (void)spg_backend_frequency_khz(&out->cpu_freq_khz);
    (void)spg_backend_throttle(&out->throttle);

    /* Scratch for the raw table. Larger than the snapshot on purpose: the
     * selection must see every process, not the first sixty-four the OS
     * happened to list. */
    static struct spg_process_sample raw[SPG_MACHINE_RAW_PROCESSES];
    size_t                           raw_n = 0u;
    uint64_t                         total = 0u;
    if (spg_backend_processes(SPG_MACHINE_RAW_PROCESSES, raw, &raw_n, &total) ==
        SPG_OK) {
        out->process_count = total;
        select_processes(raw_n, raw, prev_n, prev_procs, total_delta, profile,
                         out);
        /* Everything the backend reported was considered; anything beyond the
         * snapshot is still dropped, and the consumer must know the list is
         * not the machine. */
        if (total > (uint64_t)out->n_processes) {
            out->processes_truncated = true;
        }
    }
    return SPG_OK;
}

enum spg_status spg_machine_sample(const uint64_t               timestamp_ns,
                                   const struct spg_cpu_sample *prev,
                                   struct spg_machine_state    *out) {
    return spg_machine_sample_with_processes(timestamp_ns, prev, 0u, nullptr,
                                             nullptr, out);
}
