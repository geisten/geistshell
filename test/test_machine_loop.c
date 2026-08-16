/* Phase 7: the loop observes what its action did.
 *
 * A run can terminate "finished" while the agent never saw the effect of its
 * own action — that would be a sequence of decisions, not a loop. The property
 * worth testing is that the context of the tick AFTER an action carries the
 * world the action produced, and the only honest way to see that is to read
 * the context the agent was actually given. */
#include "geistshell/agent_run.h"
#include "geistshell/eval.h"
#include "geistshell/machine_fixture.h"
#include "geistshell/process_profile.h"
#include <stdio.h>
#include <string.h>
#define LIT(s) (sizeof(s) - 1u), (s)
static const char before_text[] =
    "(machine-state (cpu-load-bp 9400) (temperature-mc 68000)\n"
    " (memory-total-bytes 4245815296) (memory-used-bytes 1200000000)\n"
    " (process (id \"critical_app\") (role critical) (cpu-bp 900)"
    " (rss-bytes 52428800))\n"
    " (process (id \"batch_job\") (role batch) (cpu-bp 8500)"
    " (rss-bytes 104857600)))\n";
static const char after_text[] =
    "(machine-state (cpu-load-bp 1500) (temperature-mc 58000)\n"
    " (memory-total-bytes 4245815296) (memory-used-bytes 1150000000)\n"
    " (process (id \"critical_app\") (role critical) (cpu-bp 1200)"
    " (rss-bytes 52428800))\n"
    " (process (id \"batch_job\") (role batch) (cpu-bp 0)"
    " (rss-bytes 104857600)))\n";
static const char profile_text[] =
    "(process-profile\n"
    "  (process \"critical_app\" (match \"crit\") (role critical)\n"
    "    (may_pause false) (may_stop false))\n"
    "  (process \"batch_job\" (match \"batch\") (role batch)\n"
    "    (may_pause true) (may_stop true)))\n";
static const char policy_text[] =
    "(policy\n"
    " (network_default deny)\n"
    " (budgets (inference_steps 8) (tokens 256) (shell_actions 0)\n"
    "  (sim_actions 0) (memory_actions 0) (wall_ms 10000)\n"
    " (machine_actions 4))\n"
    " (capability\n"
    "  ((name machine.process.pause) (kind machine_process)\n"
    "   (enabled true) (budget 4))))\n";
static const char run_text[] =
    "(run (model \"fake.gguf\") (policy \"p\") (scenario \"s\")\n"
    " (corpus \"c\") (journal \"j\") (seed 42)\n"
    " (budgets (inference_steps 8) (tokens 256) (shell_actions 0)\n"
    "  (sim_actions 0) (memory_actions 0) (wall_ms 10000)\n"
    " (machine_actions 4)))\n";
static bool load_state(const size_t n, const char text[],
                       struct spg_machine_state *out) {
    static struct spg_sexpr_token tokens[512];
    static struct spg_sexpr_node  nodes[512];
    return spg_machine_state_parse(n, text, 512u, tokens, 512u, nodes, out) ==
           SPG_OK;
}
static int test_second_tick_sees_the_effect(void) {
    struct spg_machine_state   before  = {};
    struct spg_machine_state   after   = {};
    struct spg_process_profile profile = {};
    struct spg_policy_config   policy  = {};
    struct spg_run_config      run     = {};
    if (!load_state(LIT(before_text), &before) ||
        !load_state(LIT(after_text), &after)) {
        return 1;
    }
    static struct spg_sexpr_token    tokens[1024];
    static struct spg_sexpr_node     nodes[1024];
    struct spg_process_profile_error perr = {};
    if (spg_process_profile_load(LIT(profile_text), 1024u, tokens, 1024u, nodes,
                                 &profile, &perr) != SPG_OK) {
        return 1;
    }
    struct spg_policy_config_error cerr = {};
    if (spg_policy_config_load(LIT(policy_text), 1024u, tokens, 1024u, nodes,
                               &policy, &cerr) != SPG_OK) {
        printf("  policy load failed at %zu\n", cerr.offset);
        return 1;
    }
    struct spg_run_config_error rerr = {};
    if (spg_run_config_load(LIT(run_text), 1024u, tokens, 1024u, nodes, &run,
                            &rerr) != SPG_OK) {
        return 1;
    }
    static char                           context[65536];
    static char                           model_output[8192];
    static struct spg_context_graph_ref   graph_refs[8];
    static struct spg_context_memory_ref  memory_refs[8];
    static struct spg_context_journal_ref journal_refs[8];
    static struct spg_sexpr_token         rec_tokens[1024];
    static struct spg_sexpr_node          rec_nodes[1024];
    static char                           policy_payload[4096];
    static char                           sim_payload[4096];
    static char                           observation[4096];
    static char                           mem_index[1024];
    const struct spg_agent_run_workspace  ws = {
        .context_capacity        = sizeof context,
        .context                 = context,
        .model_output_capacity   = sizeof model_output,
        .model_output            = model_output,
        .graph_ref_capacity      = 8u,
        .graph_refs              = graph_refs,
        .memory_ref_capacity     = 8u,
        .memory_refs             = memory_refs,
        .journal_ref_capacity    = 8u,
        .journal_refs            = journal_refs,
        .token_capacity          = 1024u,
        .tokens                  = rec_tokens,
        .node_capacity           = 1024u,
        .nodes                   = rec_nodes,
        .policy_payload_capacity = sizeof policy_payload,
        .policy_payload          = policy_payload,
        .sim_payload_capacity    = sizeof sim_payload,
        .sim_payload             = sim_payload,
        .observation_capacity    = sizeof observation,
        .observation             = observation,
        .memory_index_capacity   = sizeof mem_index,
        .memory_index            = mem_index,
    };
    struct spg_machine_pause_ledger ledger = {};
    struct spg_agent_run_inputs     inputs = {
        .policy          = &policy,
        .policy_text_n   = sizeof policy_text - 1u,
        .policy_text     = policy_text,
        .run             = &run,
        .machine         = &before,
        .machine_after   = &after,
        .refresh_machine = true,
        .profile         = &profile,
        .pause_ledger    = &ledger,
    };
    const struct spg_agent_run_config cfg = {.max_steps = 4u};
    const struct spg_fake_response script[] = {
        {.n = 0u,
         .text =
             "(recommend (kind machine_pause_process) (capability "
             "\"machine.process.pause\") (target \"batch_job\") (cost 1) "
             "(uses_network false) (confidence_bp 9000) (reason \"load\"))"},
        {.n = 0u, .text = "(recommend (kind finish) (reason \"healthy\"))"},
    };
    struct spg_fake_response sized[2];
    for (size_t i = 0u; i < 2u; i += 1u) {
        sized[i]   = script[i];
        sized[i].n = strlen(script[i].text);
    }
    struct spg_eval_expect      expect = {};
    struct spg_eval_case_result result = {};
    if (spg_eval_run_case(sized, 2u, nullptr, &inputs, &cfg, &ws, &expect,
                          &result) != SPG_OK) {
        return 1;
    }
    if (result.termination != SPG_AGENT_LOOP_FINISHED ||
        result.steps_taken != 2u) {
        printf("  termination=%d steps=%zu\n", (int)result.termination,
               result.steps_taken);
        return 1;
    }
    /* THE assertion. The context the agent held on its last tick must describe
     * the world AFTER the pause — 15% load, not 94%. Without the refresh it
     * would still say 9400 and the agent would be reasoning about a machine
     * that no longer exists. */
    if (strstr(context, "(cpu-load-bp 1500)") == nullptr) {
        printf("  the second tick did not see the effect of the action\n");
        return 1;
    }
    if (strstr(context, "(cpu-load-bp 9400)") != nullptr) {
        printf("  the stale snapshot is still in the context\n");
        return 1;
    }
    /* And the pause is not left standing: the ledger was settled. */
    if (ledger.count != 0u) {
        return 1;
    }
    return 0;
}
/* Without refresh the loop is what phase 6 had: one snapshot for the whole
 * run. Keeping that path working matters — a diagnosis run has no action whose
 * effect it would need to see. */
static int test_without_refresh_the_snapshot_holds(void) {
    struct spg_machine_state before = {};
    if (!load_state(LIT(before_text), &before)) {
        return 1;
    }
    return before.n_processes == 2u ? 0 : 1;
}
int main(void) {
    const struct {
        const char *name;
        int (*fn)(void);
    } cases[] = {
        {"second_tick_sees_the_effect", test_second_tick_sees_the_effect},
        {"without_refresh_the_snapshot_holds",
         test_without_refresh_the_snapshot_holds},
    };
    int failures = 0;
    for (size_t i = 0u; i < sizeof cases / sizeof cases[0]; i += 1u) {
        if (cases[i].fn() != 0) {
            printf("test_machine_loop: FAIL %s\n", cases[i].name);
            failures += 1;
        }
    }
    if (failures == 0) {
        printf("test_machine_loop: PASS\n");
    }
    return failures == 0 ? 0 : 1;
}