/* Process telemetry: /proc/<pid>/stat parsing, per-process CPU, and the
 * selection that decides which processes reach a bounded snapshot. Pure — the
 * enumeration lives in telemetry_host.c. */

#include "geistshell/machine_state.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static constexpr uint64_t bp_scale    = 10000u;
static constexpr uint64_t counter_max = UINT64_MAX / 2u;

/* /proc/<pid>/stat field numbers, 1-based as in proc(5), counted after comm. */
static constexpr size_t field_state     = 3u;
static constexpr size_t field_utime     = 14u;
static constexpr size_t field_stime     = 15u;
static constexpr size_t field_nice      = 19u;
static constexpr size_t field_starttime = 22u;
static constexpr size_t field_rss_pages = 24u;

static bool is_digit(const char c) { return c >= '0' && c <= '9'; }

static bool scan_u64(const size_t n, const char buf[], size_t *pos,
                     uint64_t *out) {
    size_t   i     = *pos;
    uint64_t value = 0u;
    bool     any   = false;
    while (i < n && is_digit(buf[i])) {
        const uint64_t digit = (uint64_t)(buf[i] - '0');
        if (value > (counter_max - digit) / 10u) {
            return false;
        }
        value = value * 10u + digit;
        any   = true;
        i += 1u;
    }
    if (!any) {
        return false;
    }
    *pos = i;
    *out = value;
    return true;
}

/* Copy src[0..src_n) into a fixed field, truncating and always terminating.
 * Control characters become '?': a process name reaches the model context in
 * phase 3, and a newline or a quote in it would break the s-expression the
 * context is made of. */
static void copy_name(char *dst, const size_t cap, const size_t src_n,
                      const char src[]) {
    size_t i = 0u;
    for (; i + 1u < cap && i < src_n; i += 1u) {
        const char c         = src[i];
        const bool printable = c >= 0x20 && c != 0x7f;
        dst[i]               = printable ? c : '?';
    }
    dst[i] = '\0';
}

enum spg_status spg_process_parse_stat(const size_t n, const char buf[],
                                       const uint64_t             page_bytes,
                                       struct spg_process_sample *out) {
    if (out == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out = (struct spg_process_sample){
        .cpu_bp        = SPG_MACHINE_UNKNOWN,
        .rss_bytes     = SPG_MACHINE_UNKNOWN,
        .profile_index = SPG_PROCESS_NO_PROFILE,
    };

    size_t pos = 0u;
    if (!scan_u64(n, buf, &pos, &out->pid)) {
        return SPG_E_FORMAT;
    }

    /* comm is written unescaped in parentheses and may contain both spaces and
     * ')', so the only reliable anchor is the LAST ')' in the line. */
    size_t open_paren = pos;
    while (open_paren < n && buf[open_paren] != '(') {
        open_paren += 1u;
    }
    if (open_paren >= n) {
        return SPG_E_FORMAT;
    }
    size_t close_paren = n;
    for (size_t i = n; i > open_paren; i -= 1u) {
        if (buf[i - 1u] == ')') {
            close_paren = i - 1u;
            break;
        }
    }
    if (close_paren == n) {
        return SPG_E_FORMAT;
    }
    copy_name(out->name, SPG_PROCESS_NAME_CAP, close_paren - open_paren - 1u,
              buf + open_paren + 1u);

    /* Field 3 (state) onward, space-separated. */
    pos                 = close_paren + 1u;
    size_t   field      = field_state - 1u;
    bool     have_utime = false;
    bool     have_stime = false;
    uint64_t utime      = 0u;
    uint64_t stime      = 0u;
    while (pos < n && buf[pos] != '\n') {
        while (pos < n && buf[pos] == ' ') {
            pos += 1u;
        }
        if (pos >= n || buf[pos] == '\n') {
            break;
        }
        field += 1u;
        const size_t start = pos;
        while (pos < n && buf[pos] != ' ' && buf[pos] != '\n') {
            pos += 1u;
        }
        const size_t len = pos - start;
        if (len == 0u) {
            break;
        }
        size_t   scan  = start;
        uint64_t value = 0u;
        switch (field) {
        case field_state:
            out->state = buf[start];
            break;
        case field_utime:
            if (!scan_u64(n, buf, &scan, &value)) {
                return SPG_E_OVERFLOW;
            }
            utime      = value;
            have_utime = true;
            break;
        case field_stime:
            if (!scan_u64(n, buf, &scan, &value)) {
                return SPG_E_OVERFLOW;
            }
            stime      = value;
            have_stime = true;
            break;
        case field_nice: {
            const bool minus = buf[start] == '-';
            scan             = minus ? start + 1u : start;
            if (!scan_u64(n, buf, &scan, &value)) {
                return SPG_E_FORMAT;
            }
            if (value > (uint64_t)INT64_MAX) {
                return SPG_E_OVERFLOW;
            }
            out->nice = minus ? -(int64_t)value : (int64_t)value;
            break;
        }
        case field_starttime:
            if (!scan_u64(n, buf, &scan, &value)) {
                return SPG_E_OVERFLOW;
            }
            out->start_identity = value;
            break;
        case field_rss_pages:
            if (!scan_u64(n, buf, &scan, &value)) {
                return SPG_E_OVERFLOW;
            }
            if (page_bytes != 0u && value <= counter_max / page_bytes) {
                out->rss_bytes = value * page_bytes;
            }
            break;
        default:
            break;
        }
    }
    /* Anything short of starttime leaves the process without an identity, and
     * an identity-less process must never reach an action in phase 6. */
    if (field < field_starttime || !have_utime || !have_stime) {
        *out = (struct spg_process_sample){
            .cpu_bp        = SPG_MACHINE_UNKNOWN,
            .rss_bytes     = SPG_MACHINE_UNKNOWN,
            .profile_index = SPG_PROCESS_NO_PROFILE,
        };
        return SPG_E_FORMAT;
    }
    if (utime > counter_max - stime) {
        return SPG_E_OVERFLOW;
    }
    out->cpu_time = utime + stime;
    return SPG_OK;
}

uint64_t spg_process_utilisation_bp(const struct spg_process_sample *prev,
                                    const struct spg_process_sample *cur,
                                    const uint64_t total_delta) {
    if (prev == nullptr || cur == nullptr || total_delta == 0u) {
        return SPG_MACHINE_UNKNOWN;
    }
    /* Same pid but a different start time is a different process: comparing
     * their counters would invent a number. */
    if (prev->pid != cur->pid || prev->start_identity != cur->start_identity) {
        return SPG_MACHINE_UNKNOWN;
    }
    if (cur->cpu_time < prev->cpu_time) {
        return SPG_MACHINE_UNKNOWN;
    }
    const uint64_t busy = cur->cpu_time - prev->cpu_time;
    if (busy >= total_delta) {
        return bp_scale; /* multi-core: a process can exceed one CPU's share */
    }
    return busy * bp_scale / total_delta;
}

const struct spg_process_sample *
spg_process_find(const size_t n, const struct spg_process_sample procs[],
                 const uint64_t pid, const uint64_t start_identity) {
    for (size_t i = 0u; i < n; i += 1u) {
        if (procs[i].pid == pid && procs[i].start_identity == start_identity) {
            return &procs[i];
        }
    }
    return nullptr;
}

/* Strict weak ordering: managed first, then CPU, then RSS, then pid. The pid
 * rung is what makes the result independent of enumeration order. */
static bool ranks_before(const struct spg_process_sample *a,
                         const struct spg_process_sample *b) {
    const bool a_managed = a->profile_index != SPG_PROCESS_NO_PROFILE;
    const bool b_managed = b->profile_index != SPG_PROCESS_NO_PROFILE;
    if (a_managed != b_managed) {
        return a_managed;
    }
    /* Unknown CPU sorts last, never as a huge value. */
    const uint64_t a_cpu = a->cpu_bp == SPG_MACHINE_UNKNOWN ? 0u : a->cpu_bp;
    const uint64_t b_cpu = b->cpu_bp == SPG_MACHINE_UNKNOWN ? 0u : b->cpu_bp;
    if (a_cpu != b_cpu) {
        return a_cpu > b_cpu;
    }
    const uint64_t a_rss =
        a->rss_bytes == SPG_MACHINE_UNKNOWN ? 0u : a->rss_bytes;
    const uint64_t b_rss =
        b->rss_bytes == SPG_MACHINE_UNKNOWN ? 0u : b->rss_bytes;
    if (a_rss != b_rss) {
        return a_rss > b_rss;
    }
    return a->pid < b->pid;
}

bool spg_process_offer(const size_t cap, struct spg_process_sample buf[],
                       size_t *n, const struct spg_process_sample *candidate) {
    if (cap == 0u || buf == nullptr || n == nullptr || candidate == nullptr) {
        return false;
    }
    if (*n < cap) {
        buf[*n] = *candidate;
        *n += 1u;
        return true;
    }
    /* Full: drop the weakest entry, but only for something stronger. Keeping
     * the running best-k is what stops enumeration order from deciding which
     * processes the snapshot ever considered. */
    size_t worst = 0u;
    for (size_t i = 1u; i < cap; i += 1u) {
        if (ranks_before(&buf[worst], &buf[i])) {
            worst = i;
        }
    }
    if (!ranks_before(candidate, &buf[worst])) {
        return false;
    }
    buf[worst] = *candidate;
    return true;
}

enum spg_status spg_process_select(const size_t                    n_in,
                                   const struct spg_process_sample in[],
                                   const size_t                    out_cap,
                                   struct spg_process_sample       out[],
                                   size_t *out_n, bool *out_truncated) {
    if (out_n == nullptr || out_truncated == nullptr ||
        (out_cap > 0u && out == nullptr) || (n_in > 0u && in == nullptr)) {
        return SPG_E_INVALID_ARG;
    }
    *out_n         = 0u;
    *out_truncated = n_in > out_cap;

    /* Selection sort into the output: out_cap is small and bounded (64), so
     * this costs at most out_cap * n_in comparisons and needs no scratch.
     * ponytail: O(n*k) beats bringing in a sort for a 64-element ceiling. */
    bool taken_flags[SPG_MACHINE_MAX_PROCESSES] = {};
    if (n_in > SPG_MACHINE_MAX_PROCESSES) {
        /* More candidates than the flag array can track. The caller enumerates
         * into a bounded buffer, so this means it passed an oversized list. */
        return SPG_E_LIMIT;
    }
    while (*out_n < out_cap) {
        size_t best = n_in;
        for (size_t i = 0u; i < n_in; i += 1u) {
            if (taken_flags[i]) {
                continue;
            }
            if (best == n_in || ranks_before(&in[i], &in[best])) {
                best = i;
            }
        }
        if (best == n_in) {
            break;
        }
        taken_flags[best] = true;
        out[*out_n]       = in[best];
        *out_n += 1u;
    }
    return SPG_OK;
}

const char *spg_process_role_to_string(const enum spg_process_role role) {
    switch (role) {
    case SPG_PROCESS_ROLE_UNKNOWN:
        return "unknown";
    case SPG_PROCESS_ROLE_CRITICAL:
        return "critical";
    case SPG_PROCESS_ROLE_BATCH:
        return "batch";
    }
    return "unknown";
}
