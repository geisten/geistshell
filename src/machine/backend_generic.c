/* The backend for a platform nobody has ported yet.
 *
 * Every value is unknown and every call says SPG_E_UNSUPPORTED. That is not a
 * stub to be ashamed of: it is the contract the whole schema was built around
 * — a machine whose sensors cannot be read is a machine, and the agent keeps
 * running with an honest snapshot instead of a fabricated one.
 *
 * spg_backend_is_live() returns false here, which is the one thing that
 * distinguishes "this sensor is missing" from "this platform was never
 * ported". A report that cannot tell those apart is a report that will
 * eventually be misread. */

#include "geistshell/machine_backend.h"

const char *spg_backend_name(void) { return "generic"; }
bool        spg_backend_is_live(void) { return false; }

enum spg_status spg_backend_cpu(struct spg_cpu_sample *out) {
    if (out == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out = (struct spg_cpu_sample){};
    return SPG_E_UNSUPPORTED;
}

enum spg_status spg_backend_memory(struct spg_memory_sample *out) {
    if (out == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out = (struct spg_memory_sample){.total_bytes     = SPG_MACHINE_UNKNOWN,
                                      .used_bytes      = SPG_MACHINE_UNKNOWN,
                                      .swap_used_bytes = SPG_MACHINE_UNKNOWN};
    return SPG_E_UNSUPPORTED;
}

enum spg_status spg_backend_load(struct spg_load_sample *out) {
    if (out == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out = (struct spg_load_sample){.avg_1_cbp  = SPG_MACHINE_UNKNOWN,
                                    .avg_5_cbp  = SPG_MACHINE_UNKNOWN,
                                    .avg_15_cbp = SPG_MACHINE_UNKNOWN};
    return SPG_E_UNSUPPORTED;
}

enum spg_status spg_backend_temperature(int64_t *out_mc) {
    if (out_mc == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out_mc = SPG_MACHINE_UNKNOWN_S;
    return SPG_E_UNSUPPORTED;
}

enum spg_status spg_backend_frequency_khz(uint64_t *out_khz) {
    if (out_khz == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out_khz = SPG_MACHINE_UNKNOWN;
    return SPG_E_UNSUPPORTED;
}

enum spg_status spg_backend_throttle(enum spg_throttle_state *out) {
    if (out == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out = SPG_THROTTLE_UNKNOWN;
    return SPG_E_UNSUPPORTED;
}

enum spg_status spg_backend_processes(const size_t              cap,
                                      struct spg_process_sample out[],
                                      size_t *out_n, uint64_t *out_total) {
    (void)cap;
    (void)out;
    if (out_n == nullptr || out_total == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out_n     = 0u;
    *out_total = 0u;
    return SPG_E_UNSUPPORTED;
}

enum spg_status spg_backend_process_identity(const uint64_t pid,
                                             uint64_t *out_start_identity) {
    (void)pid;
    if (out_start_identity == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out_start_identity = 0u;
    /* NOT_FOUND rather than UNSUPPORTED, and deliberately so: the executor
     * refuses to signal anything it cannot identify, so an unported platform
     * must look like "this process is gone" rather than like a condition a
     * caller might decide to ignore. */
    return SPG_E_NOT_FOUND;
}

enum spg_status spg_backend_fan_read(uint64_t *out_rpm, uint64_t *out_duty) {
    if (out_rpm == nullptr || out_duty == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out_rpm  = SPG_MACHINE_UNKNOWN;
    *out_duty = SPG_MACHINE_UNKNOWN;
    return SPG_E_UNSUPPORTED;
}

enum spg_status spg_backend_fan_write(const uint64_t duty) {
    (void)duty;
    return SPG_E_UNSUPPORTED;
}
