/* Layer 2 and 3: parsing and serialisation. Pure — no I/O, no clock, no
 * allocation. Every function reads at most n bytes and tolerates input without
 * a NUL terminator. */

#include "geistshell/machine_state.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static constexpr uint64_t bp_scale    = 10000u;
static constexpr uint64_t cbp_scale   = 100u;
static constexpr uint64_t kb_to_bytes = 1024u;
/* Guard for the jiffy sums: leaves room to add another field without wrapping,
 * and no real /proc/stat comes close. */
static constexpr uint64_t counter_max = UINT64_MAX / 2u;

static bool is_digit(const char c) { return c >= '0' && c <= '9'; }

static bool is_space(const char c) { return c == ' ' || c == '\t'; }

/* Scan a decimal integer starting at *pos. Advances *pos past it. Returns
 * false on no digits at all or on overflow past counter_max. */
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

static void skip_spaces(const size_t n, const char buf[], size_t *pos) {
    while (*pos < n && is_space(buf[*pos])) {
        *pos += 1u;
    }
}

/* Start offset of the line that begins with key, or n when absent. */
static size_t find_line(const size_t n, const char buf[], const char *key) {
    const size_t key_n = strlen(key);
    size_t       i     = 0u;
    while (i < n) {
        if (n - i >= key_n && memcmp(buf + i, key, key_n) == 0) {
            return i + key_n;
        }
        while (i < n && buf[i] != '\n') {
            i += 1u;
        }
        if (i < n) {
            i += 1u;
        }
    }
    return n;
}

/* "MemTotal:   16316248 kB" -> bytes. Absent key leaves *out untouched. */
static bool read_kb_field(const size_t n, const char buf[], const char *key,
                          uint64_t *out) {
    size_t pos = find_line(n, buf, key);
    if (pos >= n) {
        return false;
    }
    skip_spaces(n, buf, &pos);
    uint64_t kb = 0u;
    if (!scan_u64(n, buf, &pos, &kb)) {
        return false;
    }
    if (kb > counter_max / kb_to_bytes) {
        return false;
    }
    *out = kb * kb_to_bytes;
    return true;
}

enum spg_status spg_telemetry_parse_stat(const size_t n, const char buf[],
                                         struct spg_cpu_sample *out) {
    if (out == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out = (struct spg_cpu_sample){};
    if (n < 4u || memcmp(buf, "cpu ", 4u) != 0) {
        return SPG_E_FORMAT;
    }
    /* user nice system idle iowait irq softirq steal guest guest_nice */
    size_t   pos    = 4u;
    uint64_t total  = 0u;
    uint64_t idle   = 0u;
    size_t   fields = 0u;
    while (pos < n && buf[pos] != '\n') {
        skip_spaces(n, buf, &pos);
        if (pos >= n || !is_digit(buf[pos])) {
            break;
        }
        uint64_t value = 0u;
        if (!scan_u64(n, buf, &pos, &value)) {
            return SPG_E_OVERFLOW;
        }
        if (total > counter_max - value) {
            return SPG_E_OVERFLOW;
        }
        total += value;
        if (fields == 3u || fields == 4u) { /* idle, iowait */
            idle += value;
        }
        fields += 1u;
    }
    /* user..idle is the minimum any kernel emits; fewer means malformed. */
    if (fields < 4u) {
        *out = (struct spg_cpu_sample){};
        return SPG_E_FORMAT;
    }
    out->idle  = idle;
    out->total = total;
    return SPG_OK;
}

enum spg_status spg_telemetry_parse_meminfo(const size_t n, const char buf[],
                                            struct spg_memory_sample *out) {
    if (out == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out = (struct spg_memory_sample){
        .total_bytes     = SPG_MACHINE_UNKNOWN,
        .used_bytes      = SPG_MACHINE_UNKNOWN,
        .swap_used_bytes = SPG_MACHINE_UNKNOWN,
    };
    uint64_t total = 0u;
    if (!read_kb_field(n, buf, "MemTotal:", &total)) {
        return SPG_E_FORMAT;
    }
    out->total_bytes = total;

    /* MemAvailable is what a workload can actually get; MemFree is not. */
    uint64_t available = 0u;
    if (read_kb_field(n, buf, "MemAvailable:", &available)) {
        out->used_bytes = available > total ? 0u : total - available;
    }
    uint64_t swap_total = 0u;
    uint64_t swap_free  = 0u;
    if (read_kb_field(n, buf, "SwapTotal:", &swap_total) &&
        read_kb_field(n, buf, "SwapFree:", &swap_free)) {
        out->swap_used_bytes =
            swap_free > swap_total ? 0u : swap_total - swap_free;
    }
    return SPG_OK;
}

/* "1.75" -> 175 centi-units. Exactly two fractional digits, truncated, so the
 * result never depends on locale or rounding mode. */
static bool scan_cbp(const size_t n, const char buf[], size_t *pos,
                     uint64_t *out) {
    uint64_t whole = 0u;
    if (!scan_u64(n, buf, pos, &whole)) {
        return false;
    }
    uint64_t frac = 0u;
    if (*pos < n && buf[*pos] == '.') {
        *pos += 1u;
        uint64_t scale = cbp_scale;
        while (*pos < n && is_digit(buf[*pos])) {
            if (scale > 1u) {
                scale /= 10u;
                frac += (uint64_t)(buf[*pos] - '0') * scale;
            }
            *pos += 1u;
        }
    }
    if (whole > (counter_max - frac) / cbp_scale) {
        return false;
    }
    *out = whole * cbp_scale + frac;
    return true;
}

enum spg_status spg_telemetry_parse_loadavg(const size_t n, const char buf[],
                                            uint64_t *out_1_cbp) {
    if (out_1_cbp == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    /* Unknown rather than 0 on a failed parse: 0 reads as a perfectly idle
     * machine, which is the opposite of "we could not tell". */
    *out_1_cbp     = SPG_MACHINE_UNKNOWN;
    size_t   pos   = 0u;
    uint64_t value = 0u;
    skip_spaces(n, buf, &pos);
    if (!scan_cbp(n, buf, &pos, &value)) {
        return SPG_E_FORMAT;
    }
    *out_1_cbp = value;
    return SPG_OK;
}

enum spg_status spg_telemetry_parse_int(const size_t n, const char buf[],
                                        int64_t *out) {
    if (out == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out             = 0;
    size_t     pos   = 0u;
    const bool minus = n > 0u && buf[0] == '-';
    if (minus) {
        pos = 1u;
    }
    uint64_t magnitude = 0u;
    if (!scan_u64(n, buf, &pos, &magnitude)) {
        return SPG_E_FORMAT;
    }
    if (magnitude > (uint64_t)INT64_MAX) {
        return SPG_E_OVERFLOW;
    }
    *out = minus ? -(int64_t)magnitude : (int64_t)magnitude;
    return SPG_OK;
}

enum spg_throttle_state spg_telemetry_parse_throttle(const size_t n,
                                                     const char   buf[]) {
    /* Pi firmware bitmask: bits 0-3 are live conditions, bits 16-19 record
     * that one occurred since boot. Anything else we cannot interpret. */
    size_t pos = 0u;
    if (n < 3u || buf[0] != '0' || (buf[1] != 'x' && buf[1] != 'X')) {
        return SPG_THROTTLE_UNKNOWN;
    }
    pos            = 2u;
    uint64_t value = 0u;
    bool     any   = false;
    for (; pos < n; pos += 1u) {
        const char c     = buf[pos];
        uint64_t   digit = 0u;
        if (is_digit(c)) {
            digit = (uint64_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = (uint64_t)(c - 'a') + 10u;
        } else if (c >= 'A' && c <= 'F') {
            digit = (uint64_t)(c - 'A') + 10u;
        } else {
            break;
        }
        if (value > (counter_max - digit) / 16u) {
            return SPG_THROTTLE_UNKNOWN;
        }
        value = value * 16u + digit;
        any   = true;
    }
    if (!any) {
        return SPG_THROTTLE_UNKNOWN;
    }
    if ((value & 0xfu) != 0u) {
        return SPG_THROTTLE_ACTIVE;
    }
    if ((value & 0xf0000u) != 0u) {
        return SPG_THROTTLE_PAST;
    }
    return SPG_THROTTLE_NONE;
}

uint64_t spg_telemetry_utilisation_bp(const struct spg_cpu_sample *prev,
                                      const struct spg_cpu_sample *cur) {
    if (prev == nullptr || cur == nullptr) {
        return SPG_MACHINE_UNKNOWN;
    }
    /* Counters only ever grow. Backwards means a reset (suspend/resume, CPU
     * hotplug): report unknown rather than a fabricated spike. */
    if (cur->total < prev->total || cur->idle < prev->idle) {
        return SPG_MACHINE_UNKNOWN;
    }
    const uint64_t total_delta = cur->total - prev->total;
    const uint64_t idle_delta  = cur->idle - prev->idle;
    if (total_delta == 0u) { /* two samples inside one tick */
        return SPG_MACHINE_UNKNOWN;
    }
    if (idle_delta >= total_delta) {
        return 0u;
    }
    const uint64_t busy = total_delta - idle_delta;
    return busy * bp_scale / total_delta;
}

const char *spg_throttle_state_to_string(const enum spg_throttle_state state) {
    switch (state) {
    case SPG_THROTTLE_UNKNOWN:
        return "unknown";
    case SPG_THROTTLE_NONE:
        return "none";
    case SPG_THROTTLE_ACTIVE:
        return "active";
    case SPG_THROTTLE_PAST:
        return "past";
    }
    return "unknown";
}

/* --- serialisation ------------------------------------------------------ */

struct writer {
    size_t capacity;
    char  *dst;
    size_t used; /* bytes that would be written, may exceed capacity */
    bool   overflowed;
};

static void put(struct writer *w, const char *text) {
    for (size_t i = 0u; text[i] != '\0'; i += 1u) {
        if (w->used + 1u < w->capacity) {
            w->dst[w->used] = text[i];
        } else {
            w->overflowed = true;
        }
        w->used += 1u;
    }
}

static void put_i64(struct writer *w, const int64_t value) {
    char     tmp[21];
    size_t   len = 0u;
    uint64_t magnitude =
        value < 0 ? (uint64_t)(-(value + 1)) + 1u : (uint64_t)value;
    do {
        tmp[len] = (char)('0' + (magnitude % 10u));
        len += 1u;
        magnitude /= 10u;
    } while (magnitude > 0u && len < sizeof tmp);
    if (value < 0) {
        put(w, "-");
    }
    char out[22];
    for (size_t i = 0u; i < len; i += 1u) {
        out[i] = tmp[len - 1u - i];
    }
    out[len] = '\0';
    put(w, out);
}

static void put_u64(struct writer *w, const uint64_t value) {
    if (value == SPG_MACHINE_UNKNOWN) {
        put(w, "unknown");
        return;
    }
    put_i64(w, (int64_t)value);
}

/* Quote a name into the s-expression. The process parser already replaced
 * control characters, but '"' and '\\' are printable and would still break the
 * form — a process could otherwise inject structure into the model context. */
static void put_quoted(struct writer *w, const char *text) {
    put(w, "\"");
    for (size_t i = 0u; text[i] != '\0'; i += 1u) {
        if (text[i] == '"' || text[i] == '\\') {
            char escaped[3] = {'\\', text[i], '\0'};
            put(w, escaped);
            continue;
        }
        const char one[2] = {text[i], '\0'};
        put(w, one);
    }
    put(w, "\"");
}

static void put_field(struct writer *w, const char *name,
                      const uint64_t value) {
    put(w, " (");
    put(w, name);
    put(w, " ");
    put_u64(w, value);
    put(w, ")");
}

enum spg_status spg_machine_state_render_masked(
    const struct spg_machine_state *state, const uint32_t ablate,
    const size_t dst_capacity, char dst[static dst_capacity],
    size_t *out_required) {
    if (state == nullptr || out_required == nullptr || dst_capacity == 0u) {
        return SPG_E_INVALID_ARG;
    }
    struct writer w = {.capacity = dst_capacity, .dst = dst};

    /* Fixed field order — the whole point of this function. An ablated field
     * is OMITTED, not blanked: writing `unknown` would measure how the model
     * copes with a dead sensor, which is a different question (#71). */
    put(&w, "(machine-state");
    if ((ablate & SPG_ABLATE_LOAD) == 0u) {
        put_field(&w, "cpu-load-bp", state->cpu_utilisation_bp);
        put_field(&w, "load-1-cbp", state->load_1_cbp);
    }
    if ((ablate & SPG_ABLATE_MEMORY) == 0u) {
        put_field(&w, "memory-total-bytes", state->memory.total_bytes);
        put_field(&w, "memory-used-bytes", state->memory.used_bytes);
        put_field(&w, "swap-used-bytes", state->memory.swap_used_bytes);
    }
    if ((ablate & SPG_ABLATE_TEMPERATURE) == 0u) {
        put(&w, " (temperature-mc ");
        if (state->temperature_mc == SPG_MACHINE_UNKNOWN_S) {
            put(&w, "unknown");
        } else {
            put_i64(&w, state->temperature_mc);
        }
        put(&w, ")");
    }
    if ((ablate & SPG_ABLATE_FREQUENCY) == 0u) {
        put_field(&w, "cpu-freq-khz", state->cpu_freq_khz);
    }
    if ((ablate & SPG_ABLATE_TEMPERATURE) == 0u) {
        /* Throttling goes with the temperature: it is the same signal seen
         * from the firmware side, and leaving it behind would make the
         * "no thermal information" variant a half-measure. */
        put(&w, " (throttle ");
        put(&w, spg_throttle_state_to_string(state->throttle));
        put(&w, ")");
    }
    put_field(&w, "process-count", state->process_count);

    for (size_t i = 0u;
         (ablate & SPG_ABLATE_PROCESSES) == 0u && i < state->n_processes;
         i += 1u) {
        const struct spg_process_sample *p = &state->processes[i];
        put(&w, " (process (id ");
        /* One shape for managed and unmanaged: the profile id when there is
         * one, the process name otherwise. Two shapes would cost a small model
         * accuracy for no gain. */
        put_quoted(&w, p->profile_id[0] != '\0' ? p->profile_id : p->name);
        put(&w, ")");
        if ((ablate & SPG_ABLATE_ROLE) == 0u) {
            put(&w, " (role ");
            put(&w, spg_process_role_to_string(p->role));
            put(&w, ")");
        }
        put_field(&w, "cpu-bp", p->cpu_bp);
        put_field(&w, "rss-bytes", p->rss_bytes);
        put(&w, ")");
    }
    if (state->processes_truncated && (ablate & SPG_ABLATE_PROCESSES) == 0u) {
        /* A list that looks complete but is not would invite wrong
         * conclusions; say how many were dropped. */
        const uint64_t shown   = (uint64_t)state->n_processes;
        const uint64_t dropped = state->process_count != SPG_MACHINE_UNKNOWN &&
                                         state->process_count > shown
                                     ? state->process_count - shown
                                     : SPG_MACHINE_UNKNOWN;
        put_field(&w, "processes-dropped", dropped);
    }
    put(&w, ")");

    *out_required = w.used + 1u;
    if (w.overflowed) {
        dst[0] = '\0'; /* no partial record ever escapes */
        return SPG_E_LIMIT;
    }
    dst[w.used] = '\0';
    return SPG_OK;
}

enum spg_status spg_machine_state_render(const struct spg_machine_state *state,
                                         const size_t dst_capacity,
                                         char         dst[static dst_capacity],
                                         size_t      *out_required) {
    return spg_machine_state_render_masked(state, SPG_ABLATE_NONE, dst_capacity,
                                           dst, out_required);
}
