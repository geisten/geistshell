/* Phase 6: typed machine actions.
 *
 * Two things are under test and they are different questions. The gate decides
 * WHETHER an action may happen — role, permissions, whether the target was even
 * observed. The executor decides whether the process it is about to signal is
 * still the one the gate decided about, because a pid can be recycled in
 * between. A test that only covered the first would miss the bug that matters
 * most. */

#include "geistshell/machine_executor.h"
#include "geistshell/policy_gate.h"
#include "geistshell/process_profile.h"

#include <stdio.h>
#include <string.h>

#define LIT(s) (sizeof(s) - 1u), (s)

static const char profile_text[] =
    "(process-profile\n"
    "  (process \"critical_app\" (match \"crit\") (role critical)\n"
    "    (may_pause false) (may_stop false))\n"
    "  (process \"batch_job\" (match \"batch\") (role batch)\n"
    "    (may_pause true) (may_stop true)))\n";

static bool load_profile(struct spg_process_profile *out) {
    static struct spg_sexpr_token    tokens[512];
    static struct spg_sexpr_node     nodes[512];
    struct spg_process_profile_error err = {};
    return spg_process_profile_load(LIT(profile_text), 512u, tokens, 512u,
                                    nodes, out, &err) == SPG_OK;
}

static struct spg_machine_state two_processes(void) {
    struct spg_machine_state s = {.n_processes = 2u};
    s.processes[0] =
        (struct spg_process_sample){.pid            = 4711u,
                                    .start_identity = 100u,
                                    .role = SPG_PROCESS_ROLE_CRITICAL};
    memcpy(s.processes[0].profile_id, "critical_app", sizeof "critical_app");
    s.processes[1] = (struct spg_process_sample){
        .pid = 4712u, .start_identity = 200u, .role = SPG_PROCESS_ROLE_BATCH};
    memcpy(s.processes[1].profile_id, "batch_job", sizeof "batch_job");
    return s;
}

/* Build a recommendation the way the parser would, so the gate sees the same
 * spans a real model output produces. */
static bool parse_rec(const char *text, struct spg_recommendation *out) {
    static struct spg_sexpr_token   tokens[256];
    static struct spg_sexpr_node    nodes[256];
    struct spg_recommendation_error err = {};
    return spg_recommendation_parse(strlen(text), text, 256u, tokens, 256u,
                                    nodes, out, &err) == SPG_OK &&
           out->state == SPG_RECOMMENDATION_VALID;
}

static const char policy_text[] =
    "(policy\n"
    " (network_default deny)\n"
    " (budgets (inference_steps 8) (tokens 256) (shell_actions 0)\n"
    "  (sim_actions 0) (memory_actions 0) (wall_ms 1000)\n"
    "  (journal_bytes 4096) (risk_bp 10000) (machine_actions 4))\n"
    " (capability\n"
    "  ((name machine.process.pause) (kind machine_process)\n"
    "   (enabled true) (budget 4))))\n";

struct gate_env {
    struct spg_policy_config   policy;
    struct spg_process_profile profile;
    struct spg_machine_state   machine;
    struct spg_policy_usage    usage;
};

static bool gate_env_init(struct gate_env *env) {
    static struct spg_sexpr_token  tokens[512];
    static struct spg_sexpr_node   nodes[512];
    struct spg_policy_config_error err = {};
    if (spg_policy_config_load(LIT(policy_text), 512u, tokens, 512u, nodes,
                               &env->policy, &err) != SPG_OK) {
        printf("  policy load failed at offset %zu\n", err.offset);
        return false;
    }
    if (!load_profile(&env->profile)) {
        return false;
    }
    env->machine = two_processes();
    env->usage   = (struct spg_policy_usage){};
    return true;
}

static enum spg_policy_deny_reason
decide(struct gate_env *env, const char *rec_text, bool with_profile) {
    struct spg_recommendation rec = {};
    if (!parse_rec(rec_text, &rec)) {
        return SPG_POLICY_DENY_INVALID_REQUEST;
    }
    char                               payload[1024];
    const struct spg_policy_gate_state state = {
        .policy_text_n         = sizeof policy_text - 1u,
        .policy_text           = policy_text,
        .recommendation_text_n = strlen(rec_text),
        .recommendation_text   = rec_text,
        .policy                = &env->policy,
        .usage                 = &env->usage,
        .profile               = with_profile ? &env->profile : nullptr,
        .machine               = with_profile ? &env->machine : nullptr,
    };
    const struct spg_policy_gate_config    config = {.actor_id = 1u};
    const struct spg_policy_gate_workspace ws     = {
        .payload_capacity = sizeof payload, .payload = payload};
    struct spg_policy_gate_result result = {};
    if (spg_policy_gate_step(&state, &config, &rec, &ws, &result) != SPG_OK) {
        return SPG_POLICY_DENY_INVALID_REQUEST;
    }
    return result.decision.kind == SPG_POLICY_DECISION_ALLOW
               ? SPG_POLICY_DENY_NONE
               : result.decision.deny_reason;
}

#define PAUSE(t)                                                               \
    "(recommend (kind machine_pause_process) "                                 \
    "(capability \"machine.process.pause\") (target \"" t "\") (cost 1) "      \
    "(uses_network false) (confidence_bp 9000) (reason \"r\"))"

static int test_allows_managed_batch(void) {
    struct gate_env env = {};
    if (!gate_env_init(&env)) {
        return 1;
    }
    return decide(&env, PAUSE("batch_job"), true) == SPG_POLICY_DENY_NONE ? 0
                                                                          : 1;
}

/* The one denial the whole phase is built around: a critical process is never
 * pausable, and the refusal comes from the policy layer, not from the executor
 * deciding to be careful. */
static int test_denies_critical(void) {
    struct gate_env env = {};
    if (!gate_env_init(&env)) {
        return 1;
    }
    const enum spg_policy_deny_reason r =
        decide(&env, PAUSE("critical_app"), true);
    if (r != SPG_POLICY_DENY_PROCESS_PROTECTED) {
        printf("  got deny reason %d\n", (int)r);
        return 1;
    }
    return 0;
}

static int test_denies_unmanaged(void) {
    struct gate_env env = {};
    if (!gate_env_init(&env)) {
        return 1;
    }
    /* Not in the profile at all: denied, not silently ignored, so the model
     * learns that unknown processes are off limits rather than inert. */
    if (decide(&env, PAUSE("sshd"), true) !=
        SPG_POLICY_DENY_UNMANAGED_PROCESS) {
        return 1;
    }
    /* No profile configured at all — the default must be denial. */
    if (decide(&env, PAUSE("batch_job"), false) !=
        SPG_POLICY_DENY_UNMANAGED_PROCESS) {
        return 1;
    }
    return 0;
}

/* Managed and permitted, but not in this tick's snapshot: acting on it would
 * mean acting on a guess. */
static int test_denies_unobserved(void) {
    struct gate_env env = {};
    if (!gate_env_init(&env)) {
        return 1;
    }
    env.machine.n_processes = 1u; /* only critical_app remains */
    return decide(&env, PAUSE("batch_job"), true) ==
                   SPG_POLICY_DENY_PROCESS_IDENTITY
               ? 0
               : 1;
}

static int test_denies_when_budget_exhausted(void) {
    struct gate_env env = {};
    if (!gate_env_init(&env)) {
        return 1;
    }
    env.usage.consumed.machine_actions = 4u; /* the whole budget */
    const enum spg_policy_deny_reason r =
        decide(&env, PAUSE("batch_job"), true);
    if (r != SPG_POLICY_DENY_GLOBAL_BUDGET &&
        r != SPG_POLICY_DENY_CAPABILITY_BUDGET) {
        printf("  got deny reason %d\n", (int)r);
        return 1;
    }
    return 0;
}

/* A protected process must not even cost budget to be refused, and the journal
 * should say why it was refused rather than that we ran out. */
static int test_protection_precedes_budget(void) {
    struct gate_env env = {};
    if (!gate_env_init(&env)) {
        return 1;
    }
    env.usage.consumed.machine_actions = 4u;
    return decide(&env, PAUSE("critical_app"), true) ==
                   SPG_POLICY_DENY_PROCESS_PROTECTED
               ? 0
               : 1;
}

static int test_denies_wrong_capability(void) {
    struct gate_env env = {};
    if (!gate_env_init(&env)) {
        return 1;
    }
    const char *rec = "(recommend (kind machine_pause_process) "
                      "(capability \"local_shell\") (target \"batch_job\") "
                      "(cost 1) (uses_network false) (confidence_bp 9000) "
                      "(reason \"r\"))";
    const enum spg_policy_deny_reason r = decide(&env, rec, true);
    if (r != SPG_POLICY_DENY_UNKNOWN_CAPABILITY &&
        r != SPG_POLICY_DENY_KIND_MISMATCH) {
        printf("  got deny reason %d\n", (int)r);
        return 1;
    }
    return 0;
}

/* The grammar refuses a machine action carrying a command. This is the
 * property that makes the action space closed: there is no field through which
 * a shell string could reach an executor. */
static int test_grammar_rejects_command(void) {
    struct spg_recommendation rec = {};
    const char               *bad =
        "(recommend (kind machine_pause_process) "
        "(capability \"machine.process.pause\") (target \"batch_job\") "
        "(command \"kill -9 4711\") (cost 1) (uses_network false) "
        "(confidence_bp 9000) (reason \"r\"))";
    if (parse_rec(bad, &rec)) {
        return 1;
    }
    /* And one without a target is equally invalid: nothing to act on. */
    const char *no_target = "(recommend (kind machine_pause_process) "
                            "(capability \"machine.process.pause\") (cost 1) "
                            "(uses_network false) (confidence_bp 9000) "
                            "(reason \"r\"))";
    return parse_rec(no_target, &rec) ? 1 : 0;
}

/* --- executor ----------------------------------------------------------- */

static enum spg_machine_exec_outcome
exec_target(const struct spg_machine_state *machine, const char *target,
            const bool enabled) {
    char                                     payload[512];
    const struct spg_machine_executor_state  st  = {.machine = machine};
    const struct spg_machine_executor_config cfg = {
        .actor_id = 1u, .execution_enabled = enabled};
    const struct spg_machine_executor_workspace ws = {
        .payload_capacity = sizeof payload, .payload = payload};
    struct spg_machine_executor_result r = {};
    if (spg_machine_executor_step(&st, &cfg, SPG_ACTION_MACHINE_PAUSE, target,
                                  &ws, &r) != SPG_OK) {
        return SPG_MACHINE_EXEC_REFUSED;
    }
    return r.outcome;
}

static int test_executor_resolves_target(void) {
    const struct spg_machine_state s = two_processes();
    /* An id the snapshot does not contain is not signalled, whatever the gate
     * said — the executor trusts the snapshot, not the recommendation. */
    if (exec_target(&s, "ghost", true) != SPG_MACHINE_EXEC_NOT_FOUND) {
        return 1;
    }
    if (exec_target(&s, "", true) != SPG_MACHINE_EXEC_NOT_FOUND) {
        return 1;
    }
    /* Execution disabled: the default for any run that has not asked to touch
     * the machine. */
    if (exec_target(&s, "batch_job", false) != SPG_MACHINE_EXEC_UNSUPPORTED) {
        return 1;
    }
    return 0;
}

/* pid 1 and the agent itself are refused by the executor even if every layer
 * above it said yes. The last line does not assume the others are perfect. */
static int test_executor_refuses_dangerous_pids(void) {
    struct spg_machine_state s = {.n_processes = 1u};
    memcpy(s.processes[0].profile_id, "batch_job", sizeof "batch_job");
    s.processes[0].pid            = 1u;
    s.processes[0].start_identity = 1u;
    const enum spg_machine_exec_outcome init_outcome =
        exec_target(&s, "batch_job", true);
#if defined(__linux__)
    if (init_outcome != SPG_MACHINE_EXEC_REFUSED) {
        return 1;
    }
#else
    /* No /proc, so the executor refuses to act at all rather than signal
     * something it cannot identify. */
    if (init_outcome != SPG_MACHINE_EXEC_UNSUPPORTED) {
        return 1;
    }
#endif
    return 0;
}

static int test_executor_null_args(void) {
    char                                        payload[64];
    const struct spg_machine_executor_state     st  = {};
    const struct spg_machine_executor_config    cfg = {};
    const struct spg_machine_executor_workspace ws  = {
        .payload_capacity = sizeof payload, .payload = payload};
    struct spg_machine_executor_result r = {};
    if (spg_machine_executor_step(&st, &cfg, SPG_ACTION_MACHINE_PAUSE, nullptr,
                                  &ws, &r) != SPG_E_INVALID_ARG) {
        return 1;
    }
    /* Only machine kinds belong here; anything else is a wiring bug upstream.
     */
    if (spg_machine_executor_step(&st, &cfg, SPG_ACTION_LOCAL_SHELL, "x", &ws,
                                  &r) != SPG_E_INVALID_ARG) {
        return 1;
    }
    size_t done = 1u;
    if (spg_machine_executor_run(&st, &cfg, 0u, nullptr, &ws, nullptr, &done) !=
            SPG_OK ||
        done != 0u) {
        return 1; /* an empty batch is legitimate */
    }
    return 0;
}

int main(void) {
    const struct {
        const char *name;
        int (*fn)(void);
    } cases[] = {
        {"allows_managed_batch", test_allows_managed_batch},
        {"denies_critical", test_denies_critical},
        {"denies_unmanaged", test_denies_unmanaged},
        {"denies_unobserved", test_denies_unobserved},
        {"denies_when_budget_exhausted", test_denies_when_budget_exhausted},
        {"protection_precedes_budget", test_protection_precedes_budget},
        {"denies_wrong_capability", test_denies_wrong_capability},
        {"grammar_rejects_command", test_grammar_rejects_command},
        {"executor_resolves_target", test_executor_resolves_target},
        {"executor_refuses_dangerous_pids",
         test_executor_refuses_dangerous_pids},
        {"executor_null_args", test_executor_null_args},
    };
    int failures = 0;
    for (size_t i = 0u; i < sizeof cases / sizeof cases[0]; i += 1u) {
        if (cases[i].fn() != 0) {
            printf("test_machine_action: FAIL %s\n", cases[i].name);
            failures += 1;
        }
    }
    if (failures == 0) {
        printf("test_machine_action: PASS\n");
    }
    return failures == 0 ? 0 : 1;
}
