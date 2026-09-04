/* The sampler against a machine that does not exist.
 *
 * Everything here was previously reachable only on a real host, where the
 * numbers are whatever the CI runner happened to be doing. The cases below are
 * the ones a developer machine will not produce on request: a counter that
 * runs backwards, a host with more processes than the snapshot holds, a sensor
 * that fails while its neighbours succeed.
 *
 * Including the fake backend replaces the platform one for this binary — see
 * the note at the top of backend_fake.c. */

#include "../src/machine/backend_fake.c"

#include "geistshell/machine_state.h"

#include <stdio.h>
#include <string.h>

static void reset(void) { spg_fake = (struct spg_backend_fake){}; }

static void put_process(const uint64_t pid, const char *name,
                        const uint64_t cpu_time, const uint64_t rss) {
    struct spg_process_sample *p = &spg_fake.processes[spg_fake.n_processes];
    *p                           = (struct spg_process_sample){
                                  .pid             = pid,
                                  .start_identity  = pid * 10u,
                                  .state           = 'S',
                                  .cpu_time        = cpu_time,
                                  .cpu_bp          = SPG_MACHINE_UNKNOWN,
                                  .rss_bytes       = rss,
                                  .profile_index   = SPG_PROCESS_NO_PROFILE,
    };
    (void)snprintf(p->name, sizeof p->name, "%s", name);
    spg_fake.n_processes += 1u;
}

/* An unported platform is not a broken one: the call fails, the snapshot stays
 * honest, and nothing above the boundary invents a number. */
static int test_not_live(void) {
    reset();
    spg_fake.not_live = true;
    spg_fake.cpu      = (struct spg_cpu_sample){.idle = 1u, .total = 2u};

    struct spg_machine_state s = {};
    if (spg_machine_sample(1000u, nullptr, &s) != SPG_E_UNSUPPORTED) {
        return 1;
    }
    if (s.timestamp_ns != 1000u || s.cpu_utilisation_bp != SPG_MACHINE_UNKNOWN ||
        s.memory.total_bytes != SPG_MACHINE_UNKNOWN || s.n_processes != 0u) {
        return 1;
    }
    return 0;
}

/* Two samples, a known delta: 30% busy. On a live host this number is whatever
 * the machine was doing between the two calls. */
static int test_utilisation_from_delta(void) {
    reset();
    spg_fake.cpu = (struct spg_cpu_sample){.idle = 700u, .total = 1000u};
    const struct spg_cpu_sample prev = {.idle = 0u, .total = 0u};

    struct spg_machine_state s = {};
    if (spg_machine_sample(1u, &prev, &s) != SPG_OK) {
        return 1;
    }
    if (s.cpu_utilisation_bp != 3000u) {
        return 1;
    }
    return 0;
}

/* Suspend/resume resets the kernel counters. The rule is unknown, not a
 * fabricated spike — untestable on a host without actually suspending it. */
static int test_counter_reset(void) {
    reset();
    spg_fake.cpu = (struct spg_cpu_sample){.idle = 5u, .total = 10u};
    const struct spg_cpu_sample prev = {.idle = 700u, .total = 1000u};

    struct spg_machine_state s = {};
    if (spg_machine_sample(1u, &prev, &s) != SPG_OK) {
        return 1;
    }
    if (s.cpu_utilisation_bp != SPG_MACHINE_UNKNOWN) {
        return 1;
    }
    return 0;
}

/* One sensor missing must not cost the others. The failing field is unknown,
 * never zero: a 0°C reading and a missing thermal zone are different machines.
 */
static int test_partial_sensors(void) {
    reset();
    spg_fake.memory = (struct spg_memory_sample){.total_bytes     = 16u << 20,
                                                 .used_bytes      = 8u << 20,
                                                 .swap_used_bytes = 0u};
    spg_fake.temperature_status = SPG_E_UNSUPPORTED;
    spg_fake.frequency_status   = SPG_E_IO;
    spg_fake.load_1_cbp         = 250u;

    struct spg_machine_state s = {};
    if (spg_machine_sample(1u, nullptr, &s) != SPG_OK) {
        return 1;
    }
    if (s.memory.total_bytes != (16u << 20) || s.load_1_cbp != 250u) {
        return 1;
    }
    if (s.temperature_mc != SPG_MACHINE_UNKNOWN_S ||
        s.cpu_freq_khz != SPG_MACHINE_UNKNOWN) {
        return 1;
    }
    return 0;
}

/* Kernel threads: no memory, no measurable CPU, no profile. They are dropped
 * before selection — the case that filled 58 of 64 context slots on a Pi. */
static int test_idle_processes_dropped(void) {
    reset();
    put_process(1u, "init", 0u, 4096u);
    for (uint64_t pid = 100u; pid < 140u; pid += 1u) {
        put_process(pid, "kworker", 0u, 0u);
    }

    struct spg_machine_state s = {};
    if (spg_machine_sample(1u, nullptr, &s) != SPG_OK) {
        return 1;
    }
    if (s.n_processes != 1u || strcmp(s.processes[0].name, "init") != 0) {
        return 1;
    }
    return 0;
}

/* A machine with more processes than the snapshot holds. The consumer must be
 * told the list is not the machine. */
static int test_truncation_reported(void) {
    reset();
    for (uint64_t i = 0u; i < SPG_MACHINE_MAX_PROCESSES + 10u; i += 1u) {
        put_process(1000u + i, "worker", 0u, (i + 1u) * 4096u);
    }
    spg_fake.total_processes = 900u; /* far beyond what was sampled */

    struct spg_machine_state s = {};
    if (spg_machine_sample(1u, nullptr, &s) != SPG_OK) {
        return 1;
    }
    if (s.n_processes != SPG_MACHINE_MAX_PROCESSES || !s.processes_truncated) {
        return 1;
    }
    if (s.process_count != 900u) {
        return 1;
    }
    return 0;
}

/* Per-process utilisation is a ratio against the machine's own delta, and a
 * recycled pid must not inherit the counter of the process it replaced. */
static int test_process_utilisation(void) {
    reset();
    spg_fake.cpu = (struct spg_cpu_sample){.idle = 0u, .total = 1000u};
    put_process(42u, "busy", 250u, 4096u);
    put_process(43u, "recycled", 900u, 4096u);

    struct spg_process_sample prev[2] = {
        {.pid            = 42u,
         .start_identity = 420u,
         .cpu_time       = 0u},
        /* same pid, different start: a different process wearing its number */
        {.pid            = 43u,
         .start_identity = 999u,
         .cpu_time       = 0u},
    };

    const struct spg_cpu_sample prev_cpu = {.idle = 0u, .total = 0u};
    struct spg_machine_state    s        = {};
    if (spg_machine_sample_with_processes(1u, &prev_cpu, 2u, prev, nullptr,
                                          &s) != SPG_OK) {
        return 1;
    }
    const struct spg_process_sample *busy =
        spg_process_find(s.n_processes, s.processes, 42u, 420u);
    const struct spg_process_sample *recycled =
        spg_process_find(s.n_processes, s.processes, 43u, 430u);
    if (busy == nullptr || recycled == nullptr) {
        return 1;
    }
    if (busy->cpu_bp != 2500u) { /* 250 of 1000 ticks */
        return 1;
    }
    if (recycled->cpu_bp != SPG_MACHINE_UNKNOWN) {
        return 1;
    }
    return 0;
}

/* The re-validation the executor does immediately before signalling. A pid the
 * machine no longer has must read as gone, never as an error to ignore. */
static int test_process_identity(void) {
    reset();
    put_process(42u, "busy", 10u, 4096u);

    uint64_t identity = 0u;
    if (spg_backend_process_identity(42u, &identity) != SPG_OK ||
        identity != 420u) {
        return 1;
    }
    if (spg_backend_process_identity(43u, &identity) != SPG_E_NOT_FOUND ||
        identity != 0u) {
        return 1;
    }
    return 0;
}

int main(void) {
    const struct {
        const char *name;
        int (*fn)(void);
    } cases[] = {
        {"not_live", test_not_live},
        {"utilisation_from_delta", test_utilisation_from_delta},
        {"counter_reset", test_counter_reset},
        {"partial_sensors", test_partial_sensors},
        {"idle_processes_dropped", test_idle_processes_dropped},
        {"truncation_reported", test_truncation_reported},
        {"process_utilisation", test_process_utilisation},
        {"process_identity", test_process_identity},
    };
    int failures = 0;
    for (size_t i = 0u; i < sizeof cases / sizeof cases[0]; i += 1u) {
        if (cases[i].fn() != 0) {
            printf("test_machine_backend_fake: FAIL %s\n", cases[i].name);
            failures += 1;
        }
    }
    if (failures == 0) {
        printf("test_machine_backend_fake: PASS\n");
    }
    return failures == 0 ? 0 : 1;
}
