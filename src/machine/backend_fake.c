/* The backend for a machine that does not exist.
 *
 * Same contract as Linux and macOS, but every value comes from a struct a test
 * sets. That is the point of the port boundary: the sampler above it — delta
 * arithmetic, selection, truncation, the "unknown is not zero" rule — can be
 * driven through cases no developer machine will produce on demand. A CPU
 * counter that goes backwards across a reboot, a host with 900 processes, a
 * sensor that reports SPG_E_IO: all of those are one assignment here.
 *
 * NOT built into libgeistshell. A test includes this file directly:
 *
 *     #include "../src/machine/backend_fake.c"
 *
 * which defines spg_backend_* in the test's own translation unit. The real
 * backend then stays inside the static library and is never pulled in — the
 * linker only takes an archive member that resolves something still missing.
 * No Makefile entry, no link-order rule to remember, and the test file itself
 * says which machine it runs against.
 *
 * Defaults are what a zeroed struct gives: SPG_OK everywhere, all counters 0,
 * no processes. A test overrides the fields its case is about and leaves the
 * rest alone.
 *
 * A getter given a failing status writes the sentinel and ignores the value
 * beside it — same as every real backend, because the sampler passes its own
 * field by pointer and does not look at the status. A fake that left a 0 there
 * instead would make "unknown is never zero" pass in the test suite and fail
 * on a host. */

#include "geistshell/machine_backend.h"

#include <string.h>

#define SPG_FAKE_MAX_PROCESSES 1024u

struct spg_backend_fake {
    bool not_live; /* inverted so the zero value is a live machine */

    enum spg_status         cpu_status;
    struct spg_cpu_sample   cpu;
    enum spg_status         memory_status;
    struct spg_memory_sample memory;
    enum spg_status         load_status;
    uint64_t                load_1_cbp;
    enum spg_status         temperature_status;
    int64_t                 temperature_mc;
    enum spg_status         frequency_status;
    uint64_t                frequency_khz;
    enum spg_status         throttle_status;
    enum spg_throttle_state throttle;

    enum spg_status           processes_status;
    struct spg_process_sample processes[SPG_FAKE_MAX_PROCESSES];
    size_t                    n_processes;
    /* How many the machine has, which is not how many are in the array above:
     * a backend reports a total larger than the snapshot it can fill, and the
     * truncation path only runs when a test can say so. 0 means "same as
     * n_processes". */
    uint64_t total_processes;

    /* spg_backend_process_identity answers from `processes` by default. Set
     * identity_status to make the pid vanish (SPG_E_NOT_FOUND) or the lookup
     * fail — the re-validation before signalling is the one caller, and it
     * must be testable without a real process to race against. */
    enum spg_status identity_status;
};

struct spg_backend_fake spg_fake = {};

const char *spg_backend_name(void) { return "fake"; }
bool        spg_backend_is_live(void) { return !spg_fake.not_live; }

enum spg_status spg_backend_cpu(struct spg_cpu_sample *out) {
    if (out == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out = spg_fake.cpu_status == SPG_OK ? spg_fake.cpu
                                         : (struct spg_cpu_sample){};
    return spg_fake.cpu_status;
}

enum spg_status spg_backend_memory(struct spg_memory_sample *out) {
    if (out == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out = spg_fake.memory_status == SPG_OK
               ? spg_fake.memory
               : (struct spg_memory_sample){
                     .total_bytes     = SPG_MACHINE_UNKNOWN,
                     .used_bytes      = SPG_MACHINE_UNKNOWN,
                     .swap_used_bytes = SPG_MACHINE_UNKNOWN};
    return spg_fake.memory_status;
}

enum spg_status spg_backend_load(uint64_t *out_1_cbp) {
    if (out_1_cbp == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out_1_cbp = spg_fake.load_status == SPG_OK ? spg_fake.load_1_cbp
                                                : SPG_MACHINE_UNKNOWN;
    return spg_fake.load_status;
}

enum spg_status spg_backend_temperature(int64_t *out_mc) {
    if (out_mc == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out_mc = spg_fake.temperature_status == SPG_OK ? spg_fake.temperature_mc
                                                    : SPG_MACHINE_UNKNOWN_S;
    return spg_fake.temperature_status;
}

enum spg_status spg_backend_frequency_khz(uint64_t *out_khz) {
    if (out_khz == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out_khz = spg_fake.frequency_status == SPG_OK ? spg_fake.frequency_khz
                                                   : SPG_MACHINE_UNKNOWN;
    return spg_fake.frequency_status;
}

enum spg_status spg_backend_throttle(enum spg_throttle_state *out) {
    if (out == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out = spg_fake.throttle_status == SPG_OK ? spg_fake.throttle
                                              : SPG_THROTTLE_UNKNOWN;
    return spg_fake.throttle_status;
}

enum spg_status spg_backend_processes(const size_t              cap,
                                      struct spg_process_sample out[],
                                      size_t *out_n, uint64_t *out_total) {
    if (out_n == nullptr || out_total == nullptr ||
        (cap > 0u && out == nullptr)) {
        return SPG_E_INVALID_ARG;
    }
    if (spg_fake.processes_status != SPG_OK) {
        *out_n     = 0u;
        *out_total = 0u;
        return spg_fake.processes_status;
    }
    size_t n = spg_fake.n_processes;
    if (n > cap) {
        n = cap;
    }
    if (n > 0u) {
        memcpy(out, spg_fake.processes, n * sizeof out[0]);
    }
    *out_n     = n;
    *out_total = spg_fake.total_processes > 0u ? spg_fake.total_processes
                                               : (uint64_t)spg_fake.n_processes;
    return spg_fake.processes_status;
}

enum spg_status spg_backend_process_identity(const uint64_t pid,
                                             uint64_t *out_start_identity) {
    if (out_start_identity == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out_start_identity = 0u;
    if (spg_fake.identity_status != SPG_OK) {
        return spg_fake.identity_status;
    }
    for (size_t i = 0u; i < spg_fake.n_processes; i += 1u) {
        if (spg_fake.processes[i].pid == pid) {
            *out_start_identity = spg_fake.processes[i].start_identity;
            return SPG_OK;
        }
    }
    /* A pid the fake machine does not have is gone, not unsupported. The
     * executor treats those differently and must never signal the first. */
    return SPG_E_NOT_FOUND;
}
