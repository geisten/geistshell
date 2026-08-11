/* macOS backend: Mach and sysctl where Linux has /proc and /sys.
 *
 * Same contract, different kernel. What is genuinely absent here is said so
 * rather than faked — macOS exposes no public temperature or fan interface,
 * and shipping a private-API dependency for an optional number would be a bad
 * trade for a runtime that has to be auditable. */

#include "geistshell/machine_backend.h"

#include <errno.h>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <stdlib.h>
#include <string.h>
#include <sys/proc_info.h>
#include <sys/sysctl.h>
#include <unistd.h>

const char *spg_backend_name(void) { return "macos"; }
bool        spg_backend_is_live(void) { return true; }

enum spg_status spg_backend_cpu(struct spg_cpu_sample *out) {
    if (out == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out = (struct spg_cpu_sample){};

    host_cpu_load_info_data_t info  = {};
    mach_msg_type_number_t    count = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO,
                        (host_info_t)&info, &count) != KERN_SUCCESS) {
        return SPG_E_IO;
    }
    /* Four buckets against Linux's ten. Only the ratio idle/total is ever
     * used, so the difference does not reach any caller. */
    /* Converted to microseconds, not left in Mach ticks: the per-process
     * counter below is a nanosecond figure, and utilisation is the ratio of
     * the two. Mixing the units pinned every process at 100% on the first
     * run — see the contract note in machine_backend.h. */
    const long     hz     = sysconf(_SC_CLK_TCK);
    const uint64_t per_us = hz > 0 ? 1000000u / (uint64_t)hz : 10000u;
    uint64_t       total  = 0u;
    for (unsigned i = 0u; i < CPU_STATE_MAX; i += 1u) {
        total += (uint64_t)info.cpu_ticks[i] * per_us;
    }
    out->idle  = (uint64_t)info.cpu_ticks[CPU_STATE_IDLE] * per_us;
    out->total = total;
    return SPG_OK;
}

enum spg_status spg_backend_memory(struct spg_memory_sample *out) {
    if (out == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out = (struct spg_memory_sample){.total_bytes     = SPG_MACHINE_UNKNOWN,
                                      .used_bytes      = SPG_MACHINE_UNKNOWN,
                                      .swap_used_bytes = SPG_MACHINE_UNKNOWN};

    uint64_t total = 0u;
    size_t   len   = sizeof total;
    if (sysctlbyname("hw.memsize", &total, &len, nullptr, 0u) == 0) {
        out->total_bytes = total;
    }

    vm_statistics64_data_t vm    = {};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vm,
                          &count) == KERN_SUCCESS &&
        out->total_bytes != SPG_MACHINE_UNKNOWN) {
        const long page = sysconf(_SC_PAGESIZE);
        if (page > 0) {
            /* Mirrors Linux's MemAvailable rather than MemFree: free plus the
             * pages the kernel would reclaim on demand. Inactive and
             * speculative pages are available in that sense; wired ones are
             * not. */
            const uint64_t available =
                ((uint64_t)vm.free_count + (uint64_t)vm.inactive_count +
                 (uint64_t)vm.purgeable_count) *
                (uint64_t)page;
            out->used_bytes = available > out->total_bytes
                                  ? 0u
                                  : out->total_bytes - available;
        }
    }

    struct xsw_usage swap = {};
    len                   = sizeof swap;
    if (sysctlbyname("vm.swapusage", &swap, &len, nullptr, 0u) == 0) {
        out->swap_used_bytes = (uint64_t)swap.xsu_used;
    }
    return SPG_OK;
}

enum spg_status spg_backend_load(struct spg_load_sample *out) {
    if (out == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out          = (struct spg_load_sample){.avg_1_cbp  = SPG_MACHINE_UNKNOWN,
                                             .avg_5_cbp  = SPG_MACHINE_UNKNOWN,
                                             .avg_15_cbp = SPG_MACHINE_UNKNOWN};
    double avg[3] = {0.0, 0.0, 0.0};
    if (getloadavg(avg, 3) != 3) {
        return SPG_E_IO;
    }
    /* The one place a float touches this codebase, converted immediately: the
     * kernel offers no integer form, and everything downstream is fixed
     * point. Truncated rather than rounded, like the Linux parser. */
    uint64_t *slots[3] = {&out->avg_1_cbp, &out->avg_5_cbp, &out->avg_15_cbp};
    for (size_t i = 0u; i < 3u; i += 1u) {
        *slots[i] = avg[i] <= 0.0 ? 0u : (uint64_t)(avg[i] * 100.0);
    }
    return SPG_OK;
}

enum spg_status spg_backend_temperature(int64_t *out_mc) {
    if (out_mc == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    /* No public interface. The SMC keys everyone uses are private and change
     * between models; a governance runtime should not depend on them for a
     * number its own schema already allows to be unknown. */
    *out_mc = SPG_MACHINE_UNKNOWN_S;
    return SPG_E_UNSUPPORTED;
}

enum spg_status spg_backend_frequency_khz(uint64_t *out_khz) {
    if (out_khz == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out_khz     = SPG_MACHINE_UNKNOWN;
    uint64_t hz  = 0u;
    size_t   len = sizeof hz;
    /* Present on Intel Macs, absent on Apple Silicon, where cores run at
     * different frequencies and no single number would be true anyway. */
    if (sysctlbyname("hw.cpufrequency", &hz, &len, nullptr, 0u) == 0 &&
        hz > 0u) {
        *out_khz = hz / 1000u;
        return SPG_OK;
    }
    return SPG_E_UNSUPPORTED;
}

enum spg_status spg_backend_throttle(enum spg_throttle_state *out) {
    if (out == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out = SPG_THROTTLE_UNKNOWN;
    return SPG_E_UNSUPPORTED;
}

/* Start time as microseconds since the epoch. Stable for the life of the
 * process and different for a recycled pid — the property phase 6 needs. */
static uint64_t start_identity_of(const struct kinfo_proc *proc) {
    return (uint64_t)proc->kp_proc.p_starttime.tv_sec * 1000000u +
           (uint64_t)proc->kp_proc.p_starttime.tv_usec;
}

static char state_of(const struct kinfo_proc *proc) {
    /* Mapped onto the letters the rest of the codebase already speaks, so a
     * consumer does not need to know which kernel produced the sample. */
    switch (proc->kp_proc.p_stat) {
    case SRUN:
        return 'R';
    case SSLEEP:
        return 'S';
    case SSTOP:
        return 'T';
    case SZOMB:
        return 'Z';
    case SIDL:
        return 'I';
    default:
        return '?';
    }
}

static void fill_sample(const struct kinfo_proc   *proc,
                        struct spg_process_sample *out) {
    *out = (struct spg_process_sample){
        .pid            = (uint64_t)proc->kp_proc.p_pid,
        .start_identity = start_identity_of(proc),
        .state          = state_of(proc),
        .nice           = (int64_t)proc->kp_proc.p_nice,
        .cpu_bp         = SPG_MACHINE_UNKNOWN,
        .rss_bytes      = SPG_MACHINE_UNKNOWN,
        .profile_index  = SPG_PROCESS_NO_PROFILE,
    };
    /* Same truncation the Linux comm has, and the same control-character
     * scrub: a process name reaches the model context, and a newline in it
     * would break the s-expression it lands in. */
    size_t i = 0u;
    for (; i + 1u < SPG_PROCESS_NAME_CAP && proc->kp_proc.p_comm[i] != '\0';
         i += 1u) {
        const char c = proc->kp_proc.p_comm[i];
        out->name[i] = (c >= 0x20 && c != 0x7f) ? c : '?';
    }
    out->name[i] = '\0';

    struct proc_taskinfo task = {};
    if (proc_pidinfo((int)out->pid, PROC_PIDTASKINFO, 0, &task, sizeof task) ==
        (int)sizeof task) {
        out->rss_bytes = task.pti_resident_size;
        /* Nanoseconds here against clock ticks on Linux. Both are raw counters
         * whose only use is a delta, so the unit stays inside the backend. */
        out->cpu_time = (task.pti_total_user + task.pti_total_system) / 1000u;
    }
}

enum spg_status spg_backend_processes(const size_t              cap,
                                      struct spg_process_sample out[],
                                      size_t *out_n, uint64_t *out_total) {
    if (out_n == nullptr || out_total == nullptr ||
        (cap > 0u && out == nullptr)) {
        return SPG_E_INVALID_ARG;
    }
    *out_n     = 0u;
    *out_total = 0u;

    int    mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0};
    size_t bytes  = 0u;
    if (sysctl(mib, 4, nullptr, &bytes, nullptr, 0u) != 0 || bytes == 0u) {
        return SPG_E_IO;
    }
    /* One allocation, immediately freed: the process table has no bounded
     * upper size the kernel will tell us in advance, and a fixed buffer would
     * either waste megabytes or silently truncate the count. */
    bytes += bytes / 8u; /* headroom: processes can appear between the calls */
    struct kinfo_proc *table = malloc(bytes);
    if (table == nullptr) {
        return SPG_E_OOM;
    }
    if (sysctl(mib, 4, table, &bytes, nullptr, 0u) != 0) {
        free(table);
        return SPG_E_IO;
    }
    const size_t count = bytes / sizeof(struct kinfo_proc);
    *out_total         = (uint64_t)count;
    for (size_t i = 0u; i < count && *out_n < cap; i += 1u) {
        if (table[i].kp_proc.p_pid <= 0) {
            continue;
        }
        fill_sample(&table[i], &out[*out_n]);
        *out_n += 1u;
    }
    free(table);
    return SPG_OK;
}

enum spg_status spg_backend_process_identity(const uint64_t pid,
                                             uint64_t *out_start_identity) {
    if (out_start_identity == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out_start_identity      = 0u;
    int               mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)pid};
    struct kinfo_proc proc   = {};
    size_t            len    = sizeof proc;
    if (sysctl(mib, 4, &proc, &len, nullptr, 0u) != 0 || len == 0u) {
        return SPG_E_NOT_FOUND;
    }
    if (proc.kp_proc.p_pid != (int)pid) {
        return SPG_E_NOT_FOUND;
    }
    *out_start_identity = start_identity_of(&proc);
    return SPG_OK;
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
