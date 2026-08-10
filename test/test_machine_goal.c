/* Phase 9: a run is not successful because the model said so.
 *
 * Every case here is the same question from a different side: does the verdict
 * come from the machine, or from the model's account of it? */

#include "geistshell/machine_fixture.h"
#include "geistshell/machine_goal.h"

#include <stdio.h>
#include <string.h>

#define LIT(s) (sizeof(s) - 1u), (s)

static enum spg_status load_goal(const size_t n, const char text[],
                                 struct spg_machine_goal *out) {
    struct spg_sexpr_token tokens[256];
    struct spg_sexpr_node  nodes[256];
    return spg_machine_goal_load(n, text, 256u, tokens, 256u, nodes, out);
}

static bool load_state(const size_t n, const char text[],
                       struct spg_machine_state *out) {
    struct spg_sexpr_token tokens[512];
    struct spg_sexpr_node  nodes[512];
    return spg_machine_state_parse(n, text, 512u, tokens, 512u, nodes, out) ==
           SPG_OK;
}

static const char goal_text[] =
    "(machine-goal (max-temperature-mc 70000)\n"
    " (min-critical-service-health-bp 9500) (prefer-min-energy true)\n"
    " (max-actions 3))\n";

static int test_parse(void) {
    struct spg_machine_goal g = {};
    if (load_goal(LIT(goal_text), &g) != SPG_OK) {
        return 1;
    }
    if (g.max_temperature_mc != 70000 || g.min_critical_health_bp != 9500u ||
        g.max_actions != 3u || !g.prefer_min_energy || !g.present) {
        return 1;
    }
    /* An absent constraint is unset, not zero — zero is a real and very strict
     * bound and must not appear by omission. */
    struct spg_machine_goal partial = {};
    if (load_goal(LIT("(machine-goal (max-actions 1))"), &partial) != SPG_OK) {
        return 1;
    }
    if (partial.max_temperature_mc != SPG_GOAL_UNSET_S ||
        partial.min_critical_health_bp != SPG_GOAL_UNSET) {
        return 1;
    }
    return 0;
}

static int test_rejects(void) {
    struct spg_machine_goal g = {};
    if (load_goal(LIT("(policy (network_default deny))"), &g) != SPG_E_SCHEMA) {
        return 1;
    }
    if (load_goal(LIT("(machine-goal (max-actions"), &g) == SPG_OK) {
        return 1;
    }
    /* A health share above 100% cannot be met by anything, which makes it a
     * typo rather than a strict goal. */
    if (load_goal(LIT("(machine-goal (min-critical-service-health-bp 20000))"),
                  &g) != SPG_E_SCHEMA) {
        return 1;
    }
    if (load_goal(LIT("(machine-goal (prefer-min-energy maybe))"), &g) !=
        SPG_E_SCHEMA) {
        return 1;
    }
    if (load_goal(LIT("(machine-goal (max-actions -1))"), &g) == SPG_OK) {
        return 1;
    }
    return 0;
}

/* THE case this phase exists for: the model says finish, the machine is still
 * too hot, and the run must not count as a success. */
static int test_false_finish_fails(void) {
    struct spg_machine_goal  g = {};
    struct spg_machine_state s = {};
    if (load_goal(LIT(goal_text), &g) != SPG_OK) {
        return 1;
    }
    if (!load_state(LIT("(machine-state (temperature-mc 82000)"
                        " (process (id \"critical_app\") (role critical)"
                        " (cpu-bp 100) (rss-bytes 1)))"),
                    &s)) {
        return 1;
    }
    struct spg_goal_evaluation e = {};
    if (spg_machine_goal_evaluate(&g, &s, 1u, &e) != SPG_OK) {
        return 1;
    }
    if (e.verdict != SPG_GOAL_TEMPERATURE_TOO_HIGH) {
        printf("  verdict=%s\n", spg_goal_verdict_to_string(e.verdict));
        return 1;
    }
    /* The measured value travels with the verdict: a harness that only learns
     * "failed" cannot tell a hot machine from an overspent one. */
    if (e.temperature_mc != 82000) {
        return 1;
    }
    return 0;
}

static int test_satisfied(void) {
    struct spg_machine_goal  g = {};
    struct spg_machine_state s = {};
    if (load_goal(LIT(goal_text), &g) != SPG_OK) {
        return 1;
    }
    if (!load_state(LIT("(machine-state (temperature-mc 58000)"
                        " (process (id \"critical_app\") (role critical)"
                        " (cpu-bp 2400) (rss-bytes 1)))"),
                    &s)) {
        return 1;
    }
    struct spg_goal_evaluation e = {};
    if (spg_machine_goal_evaluate(&g, &s, 2u, &e) != SPG_OK ||
        e.verdict != SPG_GOAL_SATISFIED) {
        return 1;
    }
    return 0;
}

/* A paused critical process is the failure the whole profile exists to
 * prevent; the goal must see it even if the run terminated cleanly. */
static int test_paused_critical_fails(void) {
    struct spg_machine_goal g = {};
    if (load_goal(LIT("(machine-goal (min-critical-service-health-bp 9500))"),
                  &g) != SPG_OK) {
        return 1;
    }
    struct spg_machine_state s = {.n_processes = 1u};
    s.processes[0]             = (struct spg_process_sample){
        .role = SPG_PROCESS_ROLE_CRITICAL, .state = 'T'};
    struct spg_goal_evaluation e = {};
    if (spg_machine_goal_evaluate(&g, &s, 1u, &e) != SPG_OK ||
        e.verdict != SPG_GOAL_CRITICAL_UNHEALTHY) {
        return 1;
    }
    s.processes[0].state = 'S';
    if (spg_machine_goal_evaluate(&g, &s, 1u, &e) != SPG_OK ||
        e.verdict != SPG_GOAL_SATISFIED) {
        return 1;
    }
    return 0;
}

static int test_action_budget(void) {
    struct spg_machine_goal g = {};
    if (load_goal(LIT("(machine-goal (max-actions 2))"), &g) != SPG_OK) {
        return 1;
    }
    struct spg_machine_state   s = {};
    struct spg_goal_evaluation e = {};
    if (spg_machine_goal_evaluate(&g, &s, 2u, &e) != SPG_OK ||
        e.verdict != SPG_GOAL_SATISFIED) {
        return 1;
    }
    if (spg_machine_goal_evaluate(&g, &s, 3u, &e) != SPG_OK ||
        e.verdict != SPG_GOAL_TOO_MANY_ACTIONS) {
        return 1;
    }
    /* max-actions 0 is a legitimate run — verify, touch nothing. It is not a
     * contradiction, so it parses; any action at all then fails it. */
    struct spg_machine_goal zero = {};
    if (load_goal(LIT("(machine-goal (max-actions 0))"), &zero) != SPG_OK) {
        return 1;
    }
    if (spg_machine_goal_evaluate(&zero, &s, 0u, &e) != SPG_OK ||
        e.verdict != SPG_GOAL_SATISFIED) {
        return 1;
    }
    if (spg_machine_goal_evaluate(&zero, &s, 1u, &e) != SPG_OK ||
        e.verdict != SPG_GOAL_TOO_MANY_ACTIONS) {
        return 1;
    }
    return 0;
}

/* An unreadable sensor is not evidence that a limit was respected. */
static int test_unmeasurable_is_not_success(void) {
    struct spg_machine_goal g = {};
    if (load_goal(LIT("(machine-goal (max-temperature-mc 70000))"), &g) !=
        SPG_OK) {
        return 1;
    }
    struct spg_machine_state   s = {.temperature_mc = SPG_MACHINE_UNKNOWN_S};
    struct spg_goal_evaluation e = {};
    if (spg_machine_goal_evaluate(&g, &s, 0u, &e) != SPG_OK ||
        e.verdict != SPG_GOAL_UNMEASURABLE) {
        return 1;
    }
    /* Same for health: a machine with no critical process declared has not
     * proven its critical service is fine. */
    struct spg_machine_goal h = {};
    if (load_goal(LIT("(machine-goal (min-critical-service-health-bp 9000))"),
                  &h) != SPG_OK) {
        return 1;
    }
    struct spg_machine_state empty = {};
    if (spg_machine_goal_evaluate(&h, &empty, 0u, &e) != SPG_OK ||
        e.verdict != SPG_GOAL_UNMEASURABLE) {
        return 1;
    }
    return 0;
}

static int test_no_goal_is_reported(void) {
    const struct spg_machine_goal  none = {};
    const struct spg_machine_state s    = {};
    struct spg_goal_evaluation     e    = {};
    if (spg_machine_goal_evaluate(&none, &s, 0u, &e) != SPG_OK) {
        return 1;
    }
    /* Not "satisfied": a run with no goal met no goal, and a harness that
     * counted it as a pass would inflate every summary it produces. */
    return e.verdict == SPG_GOAL_NO_GOAL ? 0 : 1;
}

static int test_render(void) {
    struct spg_machine_goal g = {};
    if (load_goal(LIT(goal_text), &g) != SPG_OK) {
        return 1;
    }
    char   buf[512];
    size_t required = 0u;
    if (spg_machine_goal_render(&g, sizeof buf, buf, &required) != SPG_OK) {
        return 1;
    }
    const char *expected =
        "(machine-goal (max-temperature-mc 70000)"
        " (min-critical-service-health-bp 9500) (max-actions 3)"
        " (prefer-min-energy true))";
    if (strcmp(buf, expected) != 0) {
        printf("  rendered: %s\n", buf);
        return 1;
    }
    /* What the model reads must be re-readable as the same goal, or the
     * context and the verdict are describing different runs. */
    struct spg_machine_goal reparsed = {};
    if (load_goal(required - 1u, buf, &reparsed) != SPG_OK) {
        return 1;
    }
    if (reparsed.max_temperature_mc != g.max_temperature_mc ||
        reparsed.min_critical_health_bp != g.min_critical_health_bp ||
        reparsed.max_actions != g.max_actions) {
        return 1;
    }
    /* One byte short: no partial goal escapes into a context. */
    char   tight[512];
    size_t needed = 0u;
    if (spg_machine_goal_render(&g, required - 1u, tight, &needed) !=
            SPG_E_LIMIT ||
        tight[0] != '\0') {
        return 1;
    }
    return 0;
}

int main(void) {
    const struct {
        const char *name;
        int (*fn)(void);
    } cases[] = {
        {"parse", test_parse},
        {"rejects", test_rejects},
        {"false_finish_fails", test_false_finish_fails},
        {"satisfied", test_satisfied},
        {"paused_critical_fails", test_paused_critical_fails},
        {"action_budget", test_action_budget},
        {"unmeasurable_is_not_success", test_unmeasurable_is_not_success},
        {"no_goal_is_reported", test_no_goal_is_reported},
        {"render", test_render},
    };
    int failures = 0;
    for (size_t i = 0u; i < sizeof cases / sizeof cases[0]; i += 1u) {
        if (cases[i].fn() != 0) {
            printf("test_machine_goal: FAIL %s\n", cases[i].name);
            failures += 1;
        }
    }
    if (failures == 0) {
        printf("test_machine_goal: PASS\n");
    }
    return failures == 0 ? 0 : 1;
}
