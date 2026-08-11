/* Phase 2: process telemetry, profile matching, bounded selection.
 *
 * Static fixtures only — no /proc, no dependency on what happens to run on the
 * CI machine. */

#include "geistshell/machine_backend.h"
#include "geistshell/machine_state.h"
#include "geistshell/process_profile.h"

#include <stdio.h>
#include <string.h>

#define LIT(s) (sizeof(s) - 1u), (s)
#define PAGE 4096u

/* pid (comm) state ppid pgrp ... utime(14) stime(15) ... nice(19) ...
 * starttime(22) vsize(23) rss(24) */
static const char stat_ok[] =
    "1234 (batch-worker) S 1 1234 1234 0 -1 4194304 100 0 0 0 "
    "500 250 0 0 20 5 3 0 987654 123456789 4096 "
    "18446744073709551615 1 2 3 4 5 6 7\n";

static int test_parse_basic(void) {
    struct spg_process_sample p = {};
    if (spg_process_parse_stat(LIT(stat_ok), PAGE, &p) != SPG_OK) {
        return 1;
    }
    if (p.pid != 1234u || strcmp(p.name, "batch-worker") != 0) {
        return 1;
    }
    /* field 18 priority=20, 19 nice=5, 20 num_threads=3 */
    if (p.state != 'S' || p.nice != 5 || p.start_identity != 987654u) {
        return 1;
    }
    if (p.cpu_time != 750u) { /* utime 500 + stime 250 */
        return 1;
    }
    if (p.rss_bytes != 4096ull * PAGE) {
        return 1;
    }
    /* No previous sample yet, so no CPU share. */
    if (p.cpu_bp != SPG_MACHINE_UNKNOWN) {
        return 1;
    }
    return 0;
}

/* comm is written unescaped: spaces and ')' inside it are legal and have
 * broken naive parsers for decades. */
static int test_parse_awkward_name(void) {
    const char awkward[] = "77 (weird (name) here) R 1 1 1 0 -1 0 0 0 0 0 "
                           "10 20 0 0 20 0 1 0 555 0 2 "
                           "0 0 0 0 0 0 0\n";
    struct spg_process_sample p = {};
    if (spg_process_parse_stat(LIT(awkward), PAGE, &p) != SPG_OK) {
        return 1;
    }
    if (strcmp(p.name, "weird (name) he") != 0) { /* 15 chars, kernel cap */
        printf("  name: '%s'\n", p.name);
        return 1;
    }
    if (p.pid != 77u || p.start_identity != 555u) {
        return 1;
    }
    return 0;
}

/* A name with a newline or a quote would break the s-expression this ends up
 * in during phase 3. Control characters must not survive the parser. */
static int test_parse_control_chars(void) {
    const char nasty[]          = "9 (ba\nd\tname) S 1 1 1 0 -1 0 0 0 0 0 "
                                  "1 1 0 0 20 0 1 0 42 0 1 0 0 0 0 0 0 0\n";
    struct spg_process_sample p = {};
    if (spg_process_parse_stat(LIT(nasty), PAGE, &p) != SPG_OK) {
        return 1;
    }
    if (strchr(p.name, '\n') != nullptr || strchr(p.name, '\t') != nullptr) {
        printf("  name: '%s'\n", p.name);
        return 1;
    }
    return 0;
}

static int test_parse_malformed(void) {
    struct spg_process_sample p = {};
    if (spg_process_parse_stat(LIT(""), PAGE, &p) != SPG_E_FORMAT) {
        return 1;
    }
    if (spg_process_parse_stat(LIT("notapid (x) S 1\n"), PAGE, &p) !=
        SPG_E_FORMAT) {
        return 1;
    }
    /* No closing paren at all. */
    if (spg_process_parse_stat(LIT("12 (unterminated S 1 2 3\n"), PAGE, &p) !=
        SPG_E_FORMAT) {
        return 1;
    }
    /* Truncated before starttime: without an identity the sample is unusable,
     * because phase 6 would have nothing to re-validate a pid against. */
    if (spg_process_parse_stat(LIT("12 (x) S 1 1 1 0 -1 0 0 0 0 0 1 2\n"), PAGE,
                               &p) != SPG_E_FORMAT) {
        return 1;
    }
    if (p.pid != 0u || p.start_identity != 0u) {
        return 1; /* a rejected parse must not leave a half-filled sample */
    }
    return 0;
}

static int test_parse_null(void) {
    return spg_process_parse_stat(LIT(stat_ok), PAGE, nullptr) ==
                   SPG_E_INVALID_ARG
               ? 0
               : 1;
}

static int test_utilisation(void) {
    const struct spg_process_sample a = {
        .pid = 7u, .start_identity = 100u, .cpu_time = 100u};
    const struct spg_process_sample b = {
        .pid = 7u, .start_identity = 100u, .cpu_time = 300u};
    /* 200 of 1000 ticks -> 20% */
    if (spg_process_utilisation_bp(&a, &b, 1000u) != 2000u) {
        return 1;
    }
    /* THE pid-reuse case: same pid, different start time. Comparing these
     * counters would report a wild number for a brand-new process. */
    const struct spg_process_sample reused = {
        .pid = 7u, .start_identity = 999u, .cpu_time = 300u};
    if (spg_process_utilisation_bp(&a, &reused, 1000u) != SPG_MACHINE_UNKNOWN) {
        return 1;
    }
    /* first sighting */
    if (spg_process_utilisation_bp(nullptr, &b, 1000u) != SPG_MACHINE_UNKNOWN) {
        return 1;
    }
    /* no time passed */
    if (spg_process_utilisation_bp(&a, &b, 0u) != SPG_MACHINE_UNKNOWN) {
        return 1;
    }
    /* counter went backwards */
    if (spg_process_utilisation_bp(&b, &a, 1000u) != SPG_MACHINE_UNKNOWN) {
        return 1;
    }
    /* Multi-core: a process can burn more than one CPU's worth; clamp at
     * 100% rather than report 340%. */
    const struct spg_process_sample hot = {
        .pid = 7u, .start_identity = 100u, .cpu_time = 5000u};
    if (spg_process_utilisation_bp(&a, &hot, 1000u) != 10000u) {
        return 1;
    }
    return 0;
}

static int test_find_identity(void) {
    const struct spg_process_sample procs[] = {
        {.pid = 1u, .start_identity = 10u},
        {.pid = 2u, .start_identity = 20u},
    };
    if (spg_process_find(2u, procs, 2u, 20u) != &procs[1]) {
        return 1;
    }
    /* Right pid, wrong identity: not found, not "close enough". */
    if (spg_process_find(2u, procs, 2u, 21u) != nullptr) {
        return 1;
    }
    if (spg_process_find(0u, procs, 1u, 10u) != nullptr) {
        return 1;
    }
    return 0;
}

static struct spg_process_sample mk(const uint64_t pid, const uint64_t cpu,
                                    const uint64_t rss, const bool managed) {
    return (struct spg_process_sample){
        .pid           = pid,
        .cpu_bp        = cpu,
        .rss_bytes     = rss,
        .profile_index = managed ? 0u : SPG_PROCESS_NO_PROFILE,
    };
}

static int test_select_priority(void) {
    const struct spg_process_sample in[] = {
        mk(3u, 100u, 10u, false),  /* low cpu, unmanaged */
        mk(1u, 9000u, 10u, false), /* hot, unmanaged */
        mk(2u, 50u, 10u, true),    /* managed, cold -> still first */
    };
    struct spg_process_sample out[3] = {};
    size_t                    n      = 0u;
    bool                      trunc  = true;
    if (spg_process_select(3u, in, 3u, out, &n, &trunc) != SPG_OK) {
        return 1;
    }
    if (n != 3u || trunc) {
        return 1;
    }
    if (out[0].pid != 2u || out[1].pid != 1u || out[2].pid != 3u) {
        return 1;
    }
    return 0;
}

/* /proc has no ordering guarantee. If enumeration order leaked into the
 * result, the context would differ between runs and replay would break. */
static int test_select_permutation_invariant(void) {
    const struct spg_process_sample a[] = {
        mk(3u, 100u, 10u, false), mk(1u, 9000u, 10u, false),
        mk(2u, 50u, 10u, true), mk(4u, 100u, 99u, false)};
    const struct spg_process_sample b[] = {
        mk(4u, 100u, 99u, false), mk(2u, 50u, 10u, true),
        mk(1u, 9000u, 10u, false), mk(3u, 100u, 10u, false)};
    struct spg_process_sample out_a[4] = {};
    struct spg_process_sample out_b[4] = {};
    size_t                    na = 0u, nb = 0u;
    bool                      ta = false, tb = false;
    if (spg_process_select(4u, a, 4u, out_a, &na, &ta) != SPG_OK ||
        spg_process_select(4u, b, 4u, out_b, &nb, &tb) != SPG_OK) {
        return 1;
    }
    if (na != nb) {
        return 1;
    }
    for (size_t i = 0u; i < na; i += 1u) {
        if (out_a[i].pid != out_b[i].pid) {
            printf("  order differs at %zu: %llu vs %llu\n", i,
                   (unsigned long long)out_a[i].pid,
                   (unsigned long long)out_b[i].pid);
            return 1;
        }
    }
    return 0;
}

static int test_select_truncation(void) {
    const struct spg_process_sample in[]   = {mk(1u, 100u, 0u, false),
                                              mk(2u, 900u, 0u, true),
                                              mk(3u, 500u, 0u, false)};
    struct spg_process_sample       out[2] = {};
    size_t                          n      = 0u;
    bool                            trunc  = false;
    if (spg_process_select(3u, in, 2u, out, &n, &trunc) != SPG_OK) {
        return 1;
    }
    if (n != 2u || !trunc) {
        return 1; /* dropping silently would read as "this is everything" */
    }
    if (out[0].pid != 2u || out[1].pid != 3u) {
        return 1;
    }
    /* Empty input is legitimate, not an error. */
    if (spg_process_select(0u, nullptr, 2u, out, &n, &trunc) != SPG_OK) {
        return 1;
    }
    if (n != 0u || trunc) {
        return 1;
    }
    return 0;
}

/* Unknown CPU must sort last, not as a huge number. */
static int test_select_unknown_cpu(void) {
    const struct spg_process_sample in[] = {
        mk(1u, SPG_MACHINE_UNKNOWN, 0u, false), mk(2u, 10u, 0u, false)};
    struct spg_process_sample out[2] = {};
    size_t                    n      = 0u;
    bool                      trunc  = false;
    if (spg_process_select(2u, in, 2u, out, &n, &trunc) != SPG_OK) {
        return 1;
    }
    return out[0].pid == 2u ? 0 : 1;
}

/* --- profile ------------------------------------------------------------ */

static const char profile_ok[] = "(process-profile\n"
                                 "  (process \"critical_app\"\n"
                                 "    (match \"critical-worker\")\n"
                                 "    (role critical)\n"
                                 "    (may_pause false)\n"
                                 "    (may_stop false))\n"
                                 "  (process \"batch_job\"\n"
                                 "    (match \"batch-worker\")\n"
                                 "    (role batch)\n"
                                 "    (may_pause true)\n"
                                 "    (may_stop true)))\n";

static enum spg_status load(const size_t n, const char text[],
                            struct spg_process_profile *out) {
    struct spg_sexpr_token           tokens[256];
    struct spg_sexpr_node            nodes[256];
    struct spg_process_profile_error error = {};
    return spg_process_profile_load(n, text, 256u, tokens, 256u, nodes, out,
                                    &error);
}

static int test_profile_parse(void) {
    struct spg_process_profile p = {};
    if (load(LIT(profile_ok), &p) != SPG_OK) {
        return 1;
    }
    if (p.count != 2u) {
        return 1;
    }
    if (strcmp(p.entries[0].id, "critical_app") != 0 ||
        strcmp(p.entries[0].match, "critical-worker") != 0 ||
        p.entries[0].role != SPG_PROCESS_ROLE_CRITICAL ||
        p.entries[0].may_pause || p.entries[0].may_stop) {
        return 1;
    }
    if (p.entries[1].role != SPG_PROCESS_ROLE_BATCH ||
        !p.entries[1].may_pause || !p.entries[1].may_stop) {
        return 1;
    }
    return 0;
}

static int test_profile_rejects(void) {
    struct spg_process_profile p = {};
    /* unbalanced */
    if (load(LIT("(process-profile (process \"a\" (match \"x\")"), &p) ==
        SPG_OK) {
        return 1;
    }
    /* wrong top-level form */
    if (load(LIT("(policy (network_default deny))"), &p) != SPG_E_SCHEMA) {
        return 1;
    }
    /* unknown role: a typo must not silently become "unknown" and thereby
     * strip a critical process of its protection */
    if (load(LIT("(process-profile (process \"a\" (match \"x\") "
                 "(role criticl) (may_pause false) (may_stop false)))"),
             &p) != SPG_E_SCHEMA) {
        return 1;
    }
    /* missing may_pause: permissions are never implicit */
    if (load(LIT("(process-profile (process \"a\" (match \"x\") "
                 "(role batch) (may_stop true)))"),
             &p) != SPG_E_SCHEMA) {
        return 1;
    }
    /* critical + may_pause is a contradiction, refused at parse time */
    if (load(LIT("(process-profile (process \"a\" (match \"x\") "
                 "(role critical) (may_pause true) (may_stop false)))"),
             &p) != SPG_E_SCHEMA) {
        return 1;
    }
    /* duplicate id makes a phase-6 action target ambiguous */
    if (load(LIT("(process-profile "
                 "(process \"a\" (match \"x\") (role batch) (may_pause true) "
                 "(may_stop true)) "
                 "(process \"a\" (match \"y\") (role batch) (may_pause true) "
                 "(may_stop true)))"),
             &p) != SPG_E_SCHEMA) {
        return 1;
    }
    /* a match string longer than the field cannot be truncated silently */
    if (load(LIT("(process-profile (process \"a\" (match \"aaaaaaaaaaaaaaaa"
                 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                 "aaaaaaaaaaa\") (role batch) (may_pause true) "
                 "(may_stop true)))"),
             &p) != SPG_E_SCHEMA) {
        return 1;
    }
    return 0;
}

static int test_profile_empty(void) {
    struct spg_process_profile p = {};
    /* No profile at all is a valid configuration: nothing is managed. */
    if (load(LIT("(process-profile)"), &p) != SPG_OK || p.count != 0u) {
        return 1;
    }
    if (load(LIT(""), &p) != SPG_OK || p.count != 0u) {
        return 1;
    }
    return 0;
}

static int test_profile_match(void) {
    struct spg_process_profile p = {};
    if (load(LIT(profile_ok), &p) != SPG_OK) {
        return 1;
    }
    if (spg_process_profile_match(&p, "batch-worker") != 1u) {
        return 1;
    }
    if (spg_process_profile_match(&p, "critical-worker") != 0u) {
        return 1;
    }
    /* No substring matching: "batch-worker-2" is a different process. */
    if (spg_process_profile_match(&p, "batch-worker-2") !=
        SPG_PROCESS_NO_PROFILE) {
        return 1;
    }
    if (spg_process_profile_match(&p, "sshd") != SPG_PROCESS_NO_PROFILE) {
        return 1;
    }
    if (spg_process_profile_match(&p, "") != SPG_PROCESS_NO_PROFILE) {
        return 1;
    }
    if (spg_process_profile_match(nullptr, "batch-worker") !=
        SPG_PROCESS_NO_PROFILE) {
        return 1;
    }
    return 0;
}

/* The kernel cuts comm at 15 characters. A profile naming the full binary must
 * still match, or every long-named process silently loses its role. */
static int test_profile_match_kernel_truncation(void) {
    struct spg_process_profile p = {};
    if (load(LIT("(process-profile (process \"a\" "
                 "(match \"very-long-worker-name\") (role batch) "
                 "(may_pause true) (may_stop true)))"),
             &p) != SPG_OK) {
        return 1;
    }
    if (spg_process_profile_match(&p, "very-long-worke") != 0u) {
        return 1; /* 15 chars, what /proc actually reports */
    }
    /* A shorter name that merely shares a prefix must NOT match. */
    if (spg_process_profile_match(&p, "very-long") != SPG_PROCESS_NO_PROFILE) {
        return 1;
    }
    return 0;
}

static int test_profile_first_wins(void) {
    struct spg_process_profile p = {};
    if (load(LIT("(process-profile "
                 "(process \"first\" (match \"dup\") (role critical) "
                 "(may_pause false) (may_stop false)) "
                 "(process \"second\" (match \"dup\") (role batch) "
                 "(may_pause true) (may_stop true)))"),
             &p) != SPG_OK) {
        return 1;
    }
    /* Two entries match the same name: file order decides, deterministically,
     * and the safer entry happens to be first here by construction. */
    return spg_process_profile_match(&p, "dup") == 0u ? 0 : 1;
}

static int test_apply_profile(void) {
    struct spg_process_profile p = {};
    if (load(LIT(profile_ok), &p) != SPG_OK) {
        return 1;
    }
    struct spg_process_sample procs[3] = {};
    memcpy(procs[0].name, "batch-worker", sizeof "batch-worker");
    memcpy(procs[1].name, "critical-worker", sizeof "critical-worker");
    memcpy(procs[2].name, "sshd", sizeof "sshd");
    spg_process_apply_profile(&p, 3u, procs);

    if (procs[0].role != SPG_PROCESS_ROLE_BATCH || !procs[0].may_pause) {
        return 1;
    }
    if (procs[1].role != SPG_PROCESS_ROLE_CRITICAL || procs[1].may_pause ||
        procs[1].may_stop) {
        return 1;
    }
    /* An unknown process is never implicitly pausable — this is the property
     * phase 6 relies on to deny actions on unmanaged processes. */
    if (procs[2].profile_index != SPG_PROCESS_NO_PROFILE ||
        procs[2].role != SPG_PROCESS_ROLE_UNKNOWN || procs[2].may_pause ||
        procs[2].may_stop) {
        return 1;
    }
    return 0;
}

/* The buffer can never hold every process on a real host — a Pi 5 idles at
 * ~170. Before this existed the enumerator sampled the first 64 /proc listed
 * and selected from those, so the busiest process could be invisible while
 * kernel threads at 0% filled the snapshot. That is what this pins. */
static int test_offer_keeps_best(void) {
    struct spg_process_sample buf[4] = {};
    size_t                    n      = 0u;
    /* Ten candidates, increasingly hot, offered coldest-first: the naive
     * "take the first k" would keep exactly the wrong four. */
    for (uint64_t i = 1u; i <= 10u; i += 1u) {
        const struct spg_process_sample c = mk(i, i * 100u, 0u, false);
        (void)spg_process_offer(4u, buf, &n, &c);
    }
    if (n != 4u) {
        return 1;
    }
    for (size_t i = 0u; i < n; i += 1u) {
        if (buf[i].cpu_bp < 700u) {
            printf("  kept cold process cpu_bp=%llu\n",
                   (unsigned long long)buf[i].cpu_bp);
            return 1;
        }
    }
    /* Same set offered hottest-first must keep the same four. */
    struct spg_process_sample rev[4] = {};
    size_t                    rn     = 0u;
    for (uint64_t i = 10u; i >= 1u; i -= 1u) {
        const struct spg_process_sample c = mk(i, i * 100u, 0u, false);
        (void)spg_process_offer(4u, rev, &rn, &c);
    }
    if (rn != 4u) {
        return 1;
    }
    for (size_t i = 0u; i < rn; i += 1u) {
        if (rev[i].cpu_bp < 700u) {
            return 1;
        }
    }
    /* A managed process outranks any unmanaged one, however hot. */
    struct spg_process_sample       mixed[2]     = {};
    size_t                          mn           = 0u;
    const struct spg_process_sample hot1         = mk(1u, 9000u, 0u, false);
    const struct spg_process_sample hot2         = mk(2u, 8000u, 0u, false);
    const struct spg_process_sample cold_managed = mk(3u, 1u, 0u, true);
    (void)spg_process_offer(2u, mixed, &mn, &hot1);
    (void)spg_process_offer(2u, mixed, &mn, &hot2);
    if (!spg_process_offer(2u, mixed, &mn, &cold_managed)) {
        return 1;
    }
    bool has_managed = false;
    for (size_t i = 0u; i < mn; i += 1u) {
        has_managed = has_managed || mixed[i].pid == 3u;
    }
    if (!has_managed) {
        return 1;
    }
    if (spg_process_offer(0u, buf, &n, &hot1)) {
        return 1;
    }
    return 0;
}

/* Exercises the process-aware sampler end to end. On Linux this is the only
 * test that touches real /proc; elsewhere it pins the degradation. It also
 * makes sure the entry point is actually declared in the header — a definition
 * nobody calls compiles fine and is useless. */
static int test_sample_with_processes(void) {
    struct spg_machine_state first = {};
    const enum spg_status    s1    = spg_machine_sample_with_processes(
        1u, nullptr, 0u, nullptr, nullptr, &first);
    struct spg_machine_state second = {};
    const enum spg_status    s2     = spg_machine_sample_with_processes(
        2u, &first.cpu, first.n_processes, first.processes, nullptr, &second);
    if (second.timestamp_ns != 2u) {
        return 1;
    }
    /* The backend decides, not the preprocessor: this reads the same on every
     * platform, and adding one does not mean editing the test. */
    if (spg_backend_is_live()) {
        if (s1 != SPG_OK || s2 != SPG_OK) {
            return 1;
        }
        /* Something is always running, starting with this test. */
        if (first.n_processes == 0u || second.n_processes == 0u) {
            return 1;
        }
        if (second.n_processes > SPG_MACHINE_MAX_PROCESSES) {
            return 1;
        }
        /* Every sampled process must carry an identity — phase 6 depends on
         * it, and a backend that cannot supply one is not finished. */
        for (size_t i = 0u; i < second.n_processes; i += 1u) {
            if (second.processes[i].pid == 0u ||
                second.processes[i].name[0] == '\0') {
                return 1;
            }
        }
        /* A host with more processes than the snapshot holds must say so. */
        if (second.process_count > SPG_MACHINE_MAX_PROCESSES &&
            !second.processes_truncated) {
            return 1;
        }
    } else {
        if (s1 != SPG_E_UNSUPPORTED || s2 != SPG_E_UNSUPPORTED) {
            return 1;
        }
        if (first.n_processes != 0u || second.n_processes != 0u) {
            return 1;
        }
    }
    /* A non-empty prev list with a null pointer is a caller bug, not a crash.
     */
    struct spg_machine_state ignored = {};
    if (spg_machine_sample_with_processes(3u, nullptr, 4u, nullptr, nullptr,
                                          &ignored) != SPG_E_INVALID_ARG) {
        return 1;
    }
    return 0;
}

int main(void) {
    const struct {
        const char *name;
        int (*fn)(void);
    } cases[] = {
        {"parse_basic", test_parse_basic},
        {"parse_awkward_name", test_parse_awkward_name},
        {"parse_control_chars", test_parse_control_chars},
        {"parse_malformed", test_parse_malformed},
        {"parse_null", test_parse_null},
        {"utilisation", test_utilisation},
        {"find_identity", test_find_identity},
        {"select_priority", test_select_priority},
        {"select_permutation_invariant", test_select_permutation_invariant},
        {"select_truncation", test_select_truncation},
        {"select_unknown_cpu", test_select_unknown_cpu},
        {"profile_parse", test_profile_parse},
        {"profile_rejects", test_profile_rejects},
        {"profile_empty", test_profile_empty},
        {"profile_match", test_profile_match},
        {"profile_match_kernel_truncation",
         test_profile_match_kernel_truncation},
        {"profile_first_wins", test_profile_first_wins},
        {"apply_profile", test_apply_profile},
        {"offer_keeps_best", test_offer_keeps_best},
        {"sample_with_processes", test_sample_with_processes},
    };
    int failures = 0;
    for (size_t i = 0u; i < sizeof cases / sizeof cases[0]; i += 1u) {
        if (cases[i].fn() != 0) {
            printf("test_machine_process: FAIL %s\n", cases[i].name);
            failures += 1;
        }
    }
    if (failures == 0) {
        printf("test_machine_process: PASS\n");
    }
    return failures == 0 ? 0 : 1;
}
