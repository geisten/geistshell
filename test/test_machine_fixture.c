/* Phase 4: machine-state fixtures. A diagnosis scenario is a machine state, so
 * the eval suite reads them from files instead of sampling the host. */

#include "geistshell/machine_fixture.h"

#include <stdio.h>
#include <string.h>

#define LIT(s) (sizeof(s) - 1u), (s)

static enum spg_status load(const size_t n, const char text[],
                            struct spg_machine_state *out) {
    struct spg_sexpr_token tokens[512];
    struct spg_sexpr_node  nodes[512];
    return spg_machine_state_parse(n, text, 512u, tokens, 512u, nodes, out);
}

static const char fixture[] =
    "(machine-state (cpu-load-bp 9200) (load-1-cbp 175)\n"
    " (memory-total-bytes 4245815296) (memory-used-bytes 1682931712)\n"
    " (swap-used-bytes 0) (temperature-mc 78400) (cpu-freq-khz 1500000)\n"
    " (throttle none) (process-count 167)\n"
    " (process (id \"critical_app\") (role critical) (cpu-bp 5400)"
    " (rss-bytes 4096))\n"
    " (process (id \"batch_job\") (role batch) (cpu-bp 3100)"
    " (rss-bytes 8192)))\n";

static int test_parse(void) {
    struct spg_machine_state s = {};
    if (load(LIT(fixture), &s) != SPG_OK) {
        return 1;
    }
    if (s.cpu_utilisation_bp != 9200u || s.load.avg_1_cbp != 175u ||
        s.memory.total_bytes != 4245815296u || s.temperature_mc != 78400 ||
        s.cpu_freq_khz != 1500000u || s.throttle != SPG_THROTTLE_NONE ||
        s.process_count != 167u) {
        return 1;
    }
    if (s.n_processes != 2u) {
        return 1;
    }
    if (strcmp(s.processes[0].profile_id, "critical_app") != 0 ||
        s.processes[0].role != SPG_PROCESS_ROLE_CRITICAL ||
        s.processes[0].cpu_bp != 5400u || s.processes[0].rss_bytes != 4096u) {
        return 1;
    }
    /* A managed process must outrank the rest, exactly as after a real
     * profile match — otherwise a fixture would exercise a different code
     * path than the machine does. */
    if (s.processes[0].profile_index == SPG_PROCESS_NO_PROFILE) {
        return 1;
    }
    /* A critical process is never pausable, whatever the fixture says. */
    if (s.processes[0].may_pause || s.processes[0].may_stop) {
        return 1;
    }
    if (!s.processes[1].may_pause) {
        return 1;
    }
    return 0;
}

/* The renderer and the parser are two halves of one format. If they drift, a
 * scenario stops describing what the model actually sees. */
static int test_roundtrip(void) {
    struct spg_machine_state parsed = {};
    if (load(LIT(fixture), &parsed) != SPG_OK) {
        return 1;
    }
    char   first[SPG_MACHINE_RENDER_CAP];
    size_t first_n = 0u;
    if (spg_machine_state_render(&parsed, sizeof first, first, &first_n) !=
        SPG_OK) {
        return 1;
    }
    struct spg_machine_state reparsed = {};
    if (load(first_n - 1u, first, &reparsed) != SPG_OK) {
        printf("  re-parse failed on: %s\n", first);
        return 1;
    }
    char   second[SPG_MACHINE_RENDER_CAP];
    size_t second_n = 0u;
    if (spg_machine_state_render(&reparsed, sizeof second, second, &second_n) !=
        SPG_OK) {
        return 1;
    }
    if (first_n != second_n || memcmp(first, second, first_n) != 0) {
        printf("  drift:\n   %s\n   %s\n", first, second);
        return 1;
    }
    return 0;
}

static int test_unknown_is_not_zero(void) {
    struct spg_machine_state s = {};
    /* Only two fields present: everything else must read unknown, because a
     * scenario that omits the temperature is not a scenario at 0 mC. */
    if (load(LIT("(machine-state (cpu-load-bp 100) (throttle active))"), &s) !=
        SPG_OK) {
        return 1;
    }
    if (s.cpu_utilisation_bp != 100u || s.throttle != SPG_THROTTLE_ACTIVE) {
        return 1;
    }
    if (s.memory.total_bytes != SPG_MACHINE_UNKNOWN ||
        s.temperature_mc != SPG_MACHINE_UNKNOWN_S ||
        s.cpu_freq_khz != SPG_MACHINE_UNKNOWN ||
        s.load.avg_1_cbp != SPG_MACHINE_UNKNOWN) {
        return 1;
    }
    /* An explicit `unknown` reads the same as an absent field. */
    if (load(LIT("(machine-state (cpu-load-bp unknown) (temperature-mc "
                 "unknown))"),
             &s) != SPG_OK) {
        return 1;
    }
    if (s.cpu_utilisation_bp != SPG_MACHINE_UNKNOWN ||
        s.temperature_mc != SPG_MACHINE_UNKNOWN_S) {
        return 1;
    }
    return 0;
}

static int test_negative_temperature(void) {
    struct spg_machine_state s = {};
    if (load(LIT("(machine-state (temperature-mc -5000))"), &s) != SPG_OK) {
        return 1;
    }
    return s.temperature_mc == -5000 ? 0 : 1;
}

static int test_rejects(void) {
    struct spg_machine_state s = {};
    if (load(LIT(""), &s) == SPG_OK) {
        return 1;
    }
    if (load(LIT("(policy (network_default deny))"), &s) != SPG_E_SCHEMA) {
        return 1;
    }
    if (load(LIT("(machine-state (cpu-load-bp 1"), &s) == SPG_OK) {
        return 1;
    }
    /* A misspelt role is refused, not silently demoted to unknown. */
    if (load(LIT("(machine-state (process (id \"a\") (role criticl)))"), &s) !=
        SPG_E_SCHEMA) {
        return 1;
    }
    /* A process without an id has nothing an action could target. */
    if (load(LIT("(machine-state (process (role batch) (cpu-bp 1)))"), &s) !=
        SPG_E_SCHEMA) {
        return 1;
    }
    if (load(LIT("(machine-state (cpu-load-bp notanumber))"), &s) == SPG_OK) {
        return 1;
    }
    return 0;
}

static int test_truncation_flag(void) {
    struct spg_machine_state s = {};
    if (load(LIT("(machine-state (process-count 200) (process (id \"a\") "
                 "(role batch) (cpu-bp 1) (rss-bytes 2)) "
                 "(processes-dropped 199))"),
             &s) != SPG_OK) {
        return 1;
    }
    if (!s.processes_truncated) {
        return 1;
    }
    /* Without the marker the list is complete. */
    if (load(LIT("(machine-state (process (id \"a\") (role batch)))"), &s) !=
        SPG_OK) {
        return 1;
    }
    return s.processes_truncated ? 1 : 0;
}

/* Phase 11 (#71): ablation removes exactly what it names and nothing else.
 *
 * The failure this guards against is subtle: a variant that also perturbs a
 * field it did not name makes every number in the ablation table describe two
 * changes at once, and no conclusion drawn from it would hold. */
static int test_ablation(void) {
    struct spg_machine_state s = {};
    if (load(LIT(fixture), &s) != SPG_OK) {
        return 1;
    }
    char   full[SPG_MACHINE_RENDER_CAP];
    size_t full_n = 0u;
    if (spg_machine_state_render_masked(&s, SPG_ABLATE_NONE, sizeof full, full,
                                        &full_n) != SPG_OK) {
        return 1;
    }
    /* Mask 0 is byte-identical to the unmasked renderer: one implementation,
     * so the default path cannot drift from the experiment's. */
    char   plain[SPG_MACHINE_RENDER_CAP];
    size_t plain_n = 0u;
    if (spg_machine_state_render(&s, sizeof plain, plain, &plain_n) != SPG_OK ||
        plain_n != full_n || memcmp(plain, full, full_n) != 0) {
        return 1;
    }

    const struct {
        uint32_t    bit;
        const char *removed;
        const char *kept;
    } cases[] = {
        {SPG_ABLATE_ROLE, "(role ", "(cpu-bp "},
        {SPG_ABLATE_TEMPERATURE, "(temperature-mc ", "(cpu-freq-khz "},
        {SPG_ABLATE_FREQUENCY, "(cpu-freq-khz ", "(temperature-mc "},
        {SPG_ABLATE_MEMORY, "(memory-total-bytes ", "(cpu-load-bp "},
        {SPG_ABLATE_PROCESSES, "(process (id ", "(process-count "},
        {SPG_ABLATE_LOAD, "(cpu-load-bp ", "(memory-total-bytes "},
    };
    for (size_t i = 0u; i < sizeof cases / sizeof cases[0]; i += 1u) {
        char   out[SPG_MACHINE_RENDER_CAP];
        size_t out_n = 0u;
        if (spg_machine_state_render_masked(&s, cases[i].bit, sizeof out, out,
                                            &out_n) != SPG_OK) {
            return 1;
        }
        if (strstr(out, cases[i].removed) != nullptr) {
            printf("  %s survived its own ablation\n", cases[i].removed);
            return 1;
        }
        /* A neighbouring field must be untouched, or the variant measures two
         * removals and the table means nothing. */
        if (strstr(out, cases[i].kept) == nullptr) {
            printf("  ablation of %s also removed %s\n", cases[i].removed,
                   cases[i].kept);
            return 1;
        }
        if (out_n >= full_n) {
            return 1; /* an ablation that does not shrink the block did nothing
                       */
        }
        /* Still a valid form: an ablated block the parser rejects would be a
         * broken experiment rather than a smaller context. */
        struct spg_machine_state reparsed = {};
        if (load(out_n - 1u, out, &reparsed) != SPG_OK) {
            printf("  ablated block does not parse: %s\n", out);
            return 1;
        }
    }

    /* Everything off at once: the sanity floor. It must still be a form, and
     * it must be smaller than any single ablation. */
    const uint32_t all = SPG_ABLATE_ROLE | SPG_ABLATE_TEMPERATURE |
                         SPG_ABLATE_FREQUENCY | SPG_ABLATE_MEMORY |
                         SPG_ABLATE_PROCESSES | SPG_ABLATE_LOAD;
    char           bare[SPG_MACHINE_RENDER_CAP];
    size_t         bare_n = 0u;
    if (spg_machine_state_render_masked(&s, all, sizeof bare, bare, &bare_n) !=
        SPG_OK) {
        return 1;
    }
    struct spg_machine_state reparsed = {};
    if (load(bare_n - 1u, bare, &reparsed) != SPG_OK) {
        return 1;
    }
    /* Order must not matter: a mask is a set, and a table built from runs in a
     * different order has to be the same table. */
    char   other[SPG_MACHINE_RENDER_CAP];
    size_t other_n = 0u;
    if (spg_machine_state_render_masked(&s, SPG_ABLATE_LOAD | SPG_ABLATE_ROLE,
                                        sizeof other, other,
                                        &other_n) != SPG_OK) {
        return 1;
    }
    char   swapped[SPG_MACHINE_RENDER_CAP];
    size_t swapped_n = 0u;
    if (spg_machine_state_render_masked(&s, SPG_ABLATE_ROLE | SPG_ABLATE_LOAD,
                                        sizeof swapped, swapped,
                                        &swapped_n) != SPG_OK) {
        return 1;
    }
    return (other_n == swapped_n && memcmp(other, swapped, other_n) == 0) ? 0
                                                                          : 1;
}

int main(void) {
    const struct {
        const char *name;
        int (*fn)(void);
    } cases[] = {
        {"parse", test_parse},
        {"roundtrip", test_roundtrip},
        {"unknown_is_not_zero", test_unknown_is_not_zero},
        {"negative_temperature", test_negative_temperature},
        {"rejects", test_rejects},
        {"truncation_flag", test_truncation_flag},
        {"ablation", test_ablation},
    };
    int failures = 0;
    for (size_t i = 0u; i < sizeof cases / sizeof cases[0]; i += 1u) {
        if (cases[i].fn() != 0) {
            printf("test_machine_fixture: FAIL %s\n", cases[i].name);
            failures += 1;
        }
    }
    if (failures == 0) {
        printf("test_machine_fixture: PASS\n");
    }
    return failures == 0 ? 0 : 1;
}
