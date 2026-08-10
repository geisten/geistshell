/* Phase 1 telemetry: parsers, utilisation, serialisation.
 *
 * Every case runs on static fixtures — nothing here reads /proc, so the result
 * never depends on the CI machine's load, temperature or platform. */

#include "geistshell/machine_state.h"

#include <stdio.h>
#include <string.h>

#define LIT(s) (sizeof(s) - 1u), (s)

static const char stat_ok[] = "cpu  100 20 30 800 40 0 10 0 0 0\n"
                              "cpu0 50 10 15 400 20 0 5 0 0 0\n";

static const char meminfo_ok[] = "MemTotal:       16316248 kB\n"
                                 "MemFree:         1000000 kB\n"
                                 "MemAvailable:    8158124 kB\n"
                                 "SwapTotal:       2097152 kB\n"
                                 "SwapFree:        2000000 kB\n";

static int test_stat(void) {
    struct spg_cpu_sample s = {};
    if (spg_telemetry_parse_stat(LIT(stat_ok), &s) != SPG_OK) {
        return 1;
    }
    /* total = all ten fields, idle = idle + iowait */
    if (s.total != 1000u || s.idle != 840u) {
        return 1;
    }
    return 0;
}

static int test_stat_malformed(void) {
    struct spg_cpu_sample s = {};
    /* wrong prefix */
    if (spg_telemetry_parse_stat(LIT("proc 1 2 3 4\n"), &s) != SPG_E_FORMAT) {
        return 1;
    }
    /* too few fields: user..idle is the minimum */
    if (spg_telemetry_parse_stat(LIT("cpu  1 2 3\n"), &s) != SPG_E_FORMAT) {
        return 1;
    }
    /* letters where digits belong */
    if (spg_telemetry_parse_stat(LIT("cpu  1 2 x 4 5\n"), &s) != SPG_E_FORMAT) {
        return 1;
    }
    /* empty */
    if (spg_telemetry_parse_stat(LIT(""), &s) != SPG_E_FORMAT) {
        return 1;
    }
    /* a failed parse must leave the output zeroed, not half-filled */
    if (s.total != 0u || s.idle != 0u) {
        return 1;
    }
    return 0;
}

static int test_stat_overflow(void) {
    struct spg_cpu_sample s = {};
    /* 30 nines exceeds uint64 by a wide margin */
    const char *huge = "cpu  999999999999999999999999999999 1 2 3 4\n";
    const enum spg_status status =
        spg_telemetry_parse_stat(strlen(huge), huge, &s);
    return status == SPG_E_OVERFLOW ? 0 : 1;
}

/* The parsers must not read past n even without a NUL — sized exactly so ASan
 * traps any overrun. */
static int test_stat_unterminated(void) {
    const char            src[] = "cpu  1 2 3 4 5";
    char                  exact[sizeof src - 1u];
    struct spg_cpu_sample s = {};
    memcpy(exact, src, sizeof exact);
    return spg_telemetry_parse_stat(sizeof exact, exact, &s) == SPG_OK ? 0 : 1;
}

static int test_meminfo(void) {
    struct spg_memory_sample m = {};
    if (spg_telemetry_parse_meminfo(LIT(meminfo_ok), &m) != SPG_OK) {
        return 1;
    }
    if (m.total_bytes != 16316248ull * 1024ull) {
        return 1;
    }
    if (m.used_bytes != (16316248ull - 8158124ull) * 1024ull) {
        return 1;
    }
    if (m.swap_used_bytes != (2097152ull - 2000000ull) * 1024ull) {
        return 1;
    }
    return 0;
}

static int test_meminfo_partial(void) {
    struct spg_memory_sample m = {};
    /* No MemAvailable and no swap: total known, the rest explicitly unknown. */
    if (spg_telemetry_parse_meminfo(LIT("MemTotal:  1024 kB\n"), &m) !=
        SPG_OK) {
        return 1;
    }
    if (m.total_bytes != 1024ull * 1024ull) {
        return 1;
    }
    if (m.used_bytes != SPG_MACHINE_UNKNOWN ||
        m.swap_used_bytes != SPG_MACHINE_UNKNOWN) {
        return 1;
    }
    /* Missing MemTotal is a format error, not a silent zero. */
    if (spg_telemetry_parse_meminfo(LIT("MemFree: 12 kB\n"), &m) !=
        SPG_E_FORMAT) {
        return 1;
    }
    return 0;
}

/* Available > total happens on odd kernels; used must clamp to 0, not wrap. */
static int test_meminfo_clamp(void) {
    struct spg_memory_sample m = {};
    if (spg_telemetry_parse_meminfo(
            LIT("MemTotal: 100 kB\nMemAvailable: 200 kB\n"), &m) != SPG_OK) {
        return 1;
    }
    return m.used_bytes == 0u ? 0 : 1;
}

static int test_loadavg(void) {
    struct spg_load_sample l = {};
    if (spg_telemetry_parse_loadavg(LIT("1.75 0.31 0.08 1/234 5678\n"), &l) !=
        SPG_OK) {
        return 1;
    }
    if (l.avg_1_cbp != 175u || l.avg_5_cbp != 31u || l.avg_15_cbp != 8u) {
        return 1;
    }
    /* More than two fractional digits truncate, never round — byte-identical
     * output must not depend on rounding mode. */
    if (spg_telemetry_parse_loadavg(LIT("0.999 0.0 0.0\n"), &l) != SPG_OK) {
        return 1;
    }
    if (l.avg_1_cbp != 99u) {
        return 1;
    }
    /* Integer form without a decimal point is valid. */
    if (spg_telemetry_parse_loadavg(LIT("2 3 4\n"), &l) != SPG_OK) {
        return 1;
    }
    if (l.avg_1_cbp != 200u) {
        return 1;
    }
    /* Two of three values is malformed. */
    if (spg_telemetry_parse_loadavg(LIT("1.0 2.0\n"), &l) != SPG_E_FORMAT) {
        return 1;
    }
    /* A failed parse must leave the fields unknown, never 0 — 0 would read as
     * a perfectly idle machine. */
    if (l.avg_1_cbp != SPG_MACHINE_UNKNOWN ||
        l.avg_15_cbp != SPG_MACHINE_UNKNOWN) {
        return 1;
    }
    /* An empty file is a legitimate input, not a crash. */
    if (spg_telemetry_parse_loadavg(LIT(""), &l) != SPG_E_FORMAT) {
        return 1;
    }
    return 0;
}

static int test_parse_int(void) {
    int64_t v = 0;
    if (spg_telemetry_parse_int(LIT("78400\n"), &v) != SPG_OK || v != 78400) {
        return 1;
    }
    /* Below-freezing boards exist; the sign must survive. */
    if (spg_telemetry_parse_int(LIT("-5000\n"), &v) != SPG_OK || v != -5000) {
        return 1;
    }
    if (spg_telemetry_parse_int(LIT("\n"), &v) != SPG_E_FORMAT) {
        return 1;
    }
    if (spg_telemetry_parse_int(LIT("not a number"), &v) != SPG_E_FORMAT) {
        return 1;
    }
    return 0;
}

static int test_throttle(void) {
    /* live under-voltage bit */
    if (spg_telemetry_parse_throttle(LIT("0x50005\n")) != SPG_THROTTLE_ACTIVE) {
        return 1;
    }
    /* only the "happened since boot" bits */
    if (spg_telemetry_parse_throttle(LIT("0x50000\n")) != SPG_THROTTLE_PAST) {
        return 1;
    }
    if (spg_telemetry_parse_throttle(LIT("0x0\n")) != SPG_THROTTLE_NONE) {
        return 1;
    }
    /* not hex, empty, or garbage: unknown, never a guess */
    if (spg_telemetry_parse_throttle(LIT("50005\n")) != SPG_THROTTLE_UNKNOWN) {
        return 1;
    }
    if (spg_telemetry_parse_throttle(LIT("")) != SPG_THROTTLE_UNKNOWN) {
        return 1;
    }
    if (spg_telemetry_parse_throttle(LIT("0x\n")) != SPG_THROTTLE_UNKNOWN) {
        return 1;
    }
    return 0;
}

static int test_utilisation(void) {
    const struct spg_cpu_sample a = {.idle = 800u, .total = 1000u};
    const struct spg_cpu_sample b = {.idle = 900u, .total = 2000u};
    /* 1000 total, 100 idle -> 90% busy */
    if (spg_telemetry_utilisation_bp(&a, &b) != 9000u) {
        return 1;
    }
    /* first tick: no previous sample */
    if (spg_telemetry_utilisation_bp(nullptr, &b) != SPG_MACHINE_UNKNOWN) {
        return 1;
    }
    /* two samples inside one tick: no division by zero, no fabricated value */
    if (spg_telemetry_utilisation_bp(&a, &a) != SPG_MACHINE_UNKNOWN) {
        return 1;
    }
    /* counter reset (suspend/resume): unknown, not a spike */
    if (spg_telemetry_utilisation_bp(&b, &a) != SPG_MACHINE_UNKNOWN) {
        return 1;
    }
    /* fully idle */
    const struct spg_cpu_sample c = {.idle = 1800u, .total = 2000u};
    if (spg_telemetry_utilisation_bp(&a, &c) != 0u) {
        return 1;
    }
    return 0;
}

static struct spg_machine_state sample_state(void) {
    return (struct spg_machine_state){
        .timestamp_ns       = 42u,
        .cpu_utilisation_bp = 9200u,
        .load               = {.avg_1_cbp = 175u},
        .memory             = {.total_bytes     = 1024u,
                               .used_bytes      = 512u,
                               .swap_used_bytes = 0u},
        .temperature_mc     = 78400,
        .cpu_freq_khz       = 1500000u,
        .throttle           = SPG_THROTTLE_NONE,
        .process_count      = 137u,
    };
}

static int test_render(void) {
    const struct spg_machine_state s = sample_state();
    char                           buf[512];
    size_t                         required = 0u;
    if (spg_machine_state_render(&s, sizeof buf, buf, &required) != SPG_OK) {
        return 1;
    }
    const char *expected =
        "(machine-state (cpu-load-bp 9200) (load-1-cbp 175)"
        " (memory-total-bytes 1024) (memory-used-bytes 512)"
        " (swap-used-bytes 0) (temperature-mc 78400)"
        " (cpu-freq-khz 1500000) (throttle none) (process-count 137))";
    if (strcmp(buf, expected) != 0) {
        printf("  rendered: %s\n", buf);
        return 1;
    }
    if (required != strlen(expected) + 1u) {
        return 1;
    }
    return 0;
}

static int test_render_unknown(void) {
    struct spg_machine_state s = {
        .cpu_utilisation_bp = SPG_MACHINE_UNKNOWN,
        .load               = {.avg_1_cbp  = SPG_MACHINE_UNKNOWN,
                               .avg_5_cbp  = SPG_MACHINE_UNKNOWN,
                               .avg_15_cbp = SPG_MACHINE_UNKNOWN},
        .memory             = {.total_bytes     = SPG_MACHINE_UNKNOWN,
                               .used_bytes      = SPG_MACHINE_UNKNOWN,
                               .swap_used_bytes = SPG_MACHINE_UNKNOWN},
        .temperature_mc     = SPG_MACHINE_UNKNOWN_S,
        .cpu_freq_khz       = SPG_MACHINE_UNKNOWN,
        .throttle           = SPG_THROTTLE_UNKNOWN,
        .process_count      = SPG_MACHINE_UNKNOWN,
    };
    char   buf[512];
    size_t required = 0u;
    if (spg_machine_state_render(&s, sizeof buf, buf, &required) != SPG_OK) {
        return 1;
    }
    /* Unknown is a symbol, never 0 — a consumer must not mistake a missing
     * sensor for an idle machine. */
    if (strstr(buf, "(cpu-load-bp unknown)") == nullptr ||
        strstr(buf, "(load-1-cbp unknown)") == nullptr ||
        strstr(buf, "(temperature-mc unknown)") == nullptr ||
        strstr(buf, "(throttle unknown)") == nullptr) {
        printf("  rendered: %s\n", buf);
        return 1;
    }
    if (strstr(buf, " 0)") != nullptr) {
        return 1;
    }
    return 0;
}

static int test_render_deterministic(void) {
    const struct spg_machine_state s = sample_state();
    char                           a[512];
    char                           b[512];
    size_t                         ra = 0u;
    size_t                         rb = 0u;
    if (spg_machine_state_render(&s, sizeof a, a, &ra) != SPG_OK ||
        spg_machine_state_render(&s, sizeof b, b, &rb) != SPG_OK) {
        return 1;
    }
    return (ra == rb && memcmp(a, b, ra) == 0) ? 0 : 1;
}

static int test_render_limit(void) {
    const struct spg_machine_state s = sample_state();
    char                           full[512];
    size_t                         required = 0u;
    if (spg_machine_state_render(&s, sizeof full, full, &required) != SPG_OK) {
        return 1;
    }
    /* Exactly one byte short: the boundary that off-by-ones live at. */
    char   tight[512];
    size_t needed = 0u;
    if (spg_machine_state_render(&s, required - 1u, tight, &needed) !=
        SPG_E_LIMIT) {
        return 1;
    }
    if (needed != required) {
        return 1;
    }
    /* No partial record may escape — a truncated s-expression would not
     * parse and could still reach a journal. */
    if (tight[0] != '\0') {
        return 1;
    }
    /* Exactly enough must succeed. */
    if (spg_machine_state_render(&s, required, tight, &needed) != SPG_OK) {
        return 1;
    }
    return 0;
}

static int test_null_args(void) {
    struct spg_cpu_sample s = {};
    char                  buf[64];
    size_t                required = 0u;
    if (spg_telemetry_parse_stat(LIT(stat_ok), nullptr) != SPG_E_INVALID_ARG) {
        return 1;
    }
    if (spg_telemetry_parse_meminfo(LIT(meminfo_ok), nullptr) !=
        SPG_E_INVALID_ARG) {
        return 1;
    }
    if (spg_machine_state_render(nullptr, sizeof buf, buf, &required) !=
        SPG_E_INVALID_ARG) {
        return 1;
    }
    (void)s;
    return 0;
}

/* The sampler is the only part that touches the OS. On Linux it must succeed;
 * elsewhere it must degrade to unknown rather than fail the caller's run. */
static int test_sample_platform(void) {
    struct spg_machine_state s      = {};
    const enum spg_status    status = spg_machine_sample(7u, nullptr, &s);
    if (s.timestamp_ns != 7u) {
        return 1;
    }
    /* No previous sample, so utilisation is unknown on every platform. */
    if (s.cpu_utilisation_bp != SPG_MACHINE_UNKNOWN) {
        return 1;
    }
#if defined(__linux__)
    if (status != SPG_OK) {
        return 1;
    }
    if (s.memory.total_bytes == SPG_MACHINE_UNKNOWN) {
        return 1; /* /proc/meminfo always exists on Linux */
    }
#else
    if (status != SPG_E_UNSUPPORTED) {
        return 1;
    }
    if (s.memory.total_bytes != SPG_MACHINE_UNKNOWN ||
        s.temperature_mc != SPG_MACHINE_UNKNOWN_S ||
        s.throttle != SPG_THROTTLE_UNKNOWN) {
        return 1;
    }
#endif
    if (spg_machine_sample(1u, nullptr, nullptr) != SPG_E_INVALID_ARG) {
        return 1;
    }
    return 0;
}

int main(void) {
    const struct {
        const char *name;
        int (*fn)(void);
    } cases[] = {
        {"stat", test_stat},
        {"stat_malformed", test_stat_malformed},
        {"stat_overflow", test_stat_overflow},
        {"stat_unterminated", test_stat_unterminated},
        {"meminfo", test_meminfo},
        {"meminfo_partial", test_meminfo_partial},
        {"meminfo_clamp", test_meminfo_clamp},
        {"loadavg", test_loadavg},
        {"parse_int", test_parse_int},
        {"throttle", test_throttle},
        {"utilisation", test_utilisation},
        {"render", test_render},
        {"render_unknown", test_render_unknown},
        {"render_deterministic", test_render_deterministic},
        {"render_limit", test_render_limit},
        {"null_args", test_null_args},
        {"sample_platform", test_sample_platform},
    };
    int failures = 0;
    for (size_t i = 0u; i < sizeof cases / sizeof cases[0]; i += 1u) {
        if (cases[i].fn() != 0) {
            printf("test_machine_telemetry: FAIL %s\n", cases[i].name);
            failures += 1;
        }
    }
    if (failures == 0) {
        printf("test_machine_telemetry: PASS\n");
    }
    return failures == 0 ? 0 : 1;
}
