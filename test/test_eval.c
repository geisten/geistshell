#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#    define _DARWIN_C_SOURCE 1
#endif

#include "geistshell/eval.h"

#include "geistshell/policy_config.h"
#include "geistshell/run_config.h"
#include "geistshell/sim_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char policy_text[] =
    "(policy (network_default deny)"
    " (budgets (inference_steps 100) (tokens 4096) (shell_actions 8)"
    "  (sim_actions 8) (wall_ms 60000))"
    " (capability ((name sim.act) (kind simulator) (enabled true) (budget 8))))";

static const char scenario_text[] =
    "(scenario (host (id web) (criticality_bp 8000))"
    " (service (id s) (host web) (name ssh) (port 22) (exposure_bp 5000))"
    " (vulnerability (id v) (service s) (severity_bp 8000) (patched false)))";

static const char run_text[] =
    "(run (model \"fake\") (policy \"p\") (scenario \"s\") (corpus \"c\")"
    " (journal \"j\") (seed 42)"
    " (budgets (inference_steps 100) (tokens 4096) (shell_actions 8)"
    "  (sim_actions 8) (wall_ms 60000)))";

#define FR(s) ((struct spg_fake_response){.n = sizeof(s) - 1u, .text = (s)})

struct fixture {
    struct spg_policy_config policy;
    struct spg_sim_config    sim;
    struct spg_run_config    run;

    struct spg_context_graph_ref   graph_refs[8];
    struct spg_context_memory_ref  memory_refs[8];
    struct spg_context_journal_ref journal_refs[8];
    char                           context[8192];
    char                           model_output[1024];
    struct spg_sexpr_token         tokens[128];
    struct spg_sexpr_node          nodes[128];
    char                           policy_payload[1024];
    char                           sim_payload[1024];
    char                           observation[4096];
    char                           shell_stdout[4096];
    char                           shell_stderr[1024];
    char                           mem_index[4096];
    struct spg_journal_record_header trajectory[256];
};

static int load_fixture(struct fixture *fx) {
    *fx = (struct fixture){};
    struct spg_sexpr_token tok[256];
    struct spg_sexpr_node  nod[256];
    struct spg_policy_config_error pe = {};
    struct spg_sim_config_error    se = {};
    struct spg_run_config_error    re = {};
    if (spg_policy_config_load(strlen(policy_text), policy_text, 256u, tok, 256u,
                               nod, &fx->policy, &pe) != SPG_OK) {
        return 1;
    }
    if (spg_sim_config_load(strlen(scenario_text), scenario_text, 256u, tok,
                            256u, nod, &fx->sim, &se) != SPG_OK) {
        return 1;
    }
    if (spg_run_config_load(strlen(run_text), run_text, 256u, tok, 256u, nod,
                            &fx->run, &re) != SPG_OK) {
        return 1;
    }
    return 0;
}

static struct spg_agent_run_workspace ws_for(struct fixture *fx) {
    return (struct spg_agent_run_workspace){
        .context_capacity        = sizeof fx->context,
        .context                 = fx->context,
        .model_output_capacity   = sizeof fx->model_output,
        .model_output            = fx->model_output,
        .graph_ref_capacity      = 8u,
        .graph_refs              = fx->graph_refs,
        .memory_ref_capacity     = 8u,
        .memory_refs             = fx->memory_refs,
        .journal_ref_capacity    = 8u,
        .journal_refs            = fx->journal_refs,
        .token_capacity          = 128u,
        .tokens                  = fx->tokens,
        .node_capacity           = 128u,
        .nodes                   = fx->nodes,
        .policy_payload_capacity = sizeof fx->policy_payload,
        .policy_payload          = fx->policy_payload,
        .sim_payload_capacity    = sizeof fx->sim_payload,
        .sim_payload             = fx->sim_payload,
        .observation_capacity    = sizeof fx->observation,
        .observation             = fx->observation,
        .shell_stdout_capacity   = sizeof fx->shell_stdout,
        .shell_stdout            = fx->shell_stdout,
        .shell_stderr_capacity   = sizeof fx->shell_stderr,
        .shell_stderr            = fx->shell_stderr,
        .trajectory_capacity     = 256u,
        .trajectory              = fx->trajectory,
        .memory_index_capacity   = sizeof fx->mem_index,
        .memory_index            = fx->mem_index,
    };
}

static const struct spg_fake_response sim_finish[] = {
    FR("(recommend (kind simulator) (capability \"sim.act\") (cost 1) "
       "(uses_network false) (confidence_bp 7000) (reason \"r\"))"),
    FR("(recommend (kind finish) (reason \"done\"))"),
};

static int test_case_pass(void) {
    struct fixture fx;
    if (load_fixture(&fx) != 0) {
        return 1;
    }
    const struct spg_agent_run_inputs inputs = {
        .policy        = &fx.policy,
        .policy_text_n = strlen(policy_text),
        .policy_text   = policy_text,
        .run           = &fx.run,
        .sim           = &fx.sim,
    };
    const struct spg_agent_run_config config = {.max_steps = 5u};
    const struct spg_agent_run_workspace ws = ws_for(&fx);
    const struct spg_eval_expect expect = {.check_termination = true,
                                           .termination = SPG_AGENT_LOOP_FINISHED,
                                           .min_steps   = 2u,
                                           .max_steps   = 2u};
    struct spg_eval_case_result res = {};
    if (spg_eval_run_case(sim_finish, 2u, nullptr, &inputs, &config, &ws,
                          &expect, &res) != SPG_OK) {
        return 1;
    }
    return (res.outcome == SPG_EVAL_PASS &&
            res.termination == SPG_AGENT_LOOP_FINISHED && res.steps_taken == 2u)
               ? 0
               : 1;
}

static int test_case_fail_termination(void) {
    struct fixture fx;
    if (load_fixture(&fx) != 0) {
        return 1;
    }
    const struct spg_agent_run_inputs inputs = {
        .policy        = &fx.policy,
        .policy_text_n = strlen(policy_text),
        .policy_text   = policy_text,
        .run           = &fx.run,
        .sim           = &fx.sim,
    };
    /* cap the loop at 1 step: it will not reach finish -> MAX_STEPS */
    const struct spg_agent_run_config config = {.max_steps = 1u};
    const struct spg_agent_run_workspace ws = ws_for(&fx);
    const struct spg_eval_expect expect = {.check_termination = true,
                                           .termination =
                                               SPG_AGENT_LOOP_FINISHED};
    struct spg_eval_case_result res = {};
    if (spg_eval_run_case(sim_finish, 2u, nullptr, &inputs, &config, &ws,
                          &expect, &res) != SPG_OK) {
        return 1;
    }
    return (res.outcome == SPG_EVAL_FAIL_TERMINATION &&
            res.termination == SPG_AGENT_LOOP_MAX_STEPS)
               ? 0
               : 1;
}

/* A memory in the store is rendered into the agent's context via the index. */
static int test_memory_index_injected(void) {
    struct fixture fx;
    if (load_fixture(&fx) != 0) {
        return 1;
    }
    char dir[64];
    memcpy(dir, "/tmp/spg_evalmem_XXXXXX", 24u);
    struct spg_mem_store store;
    if (mkdtemp(dir) == nullptr || spg_mem_store_open(&store, dir) != SPG_OK ||
        spg_mem_save(&store, "factslug", "a saved fact", "the body") !=
            SPG_OK) {
        return 1;
    }
    const struct spg_agent_run_inputs inputs = {
        .policy        = &fx.policy,
        .policy_text_n = strlen(policy_text),
        .policy_text   = policy_text,
        .run           = &fx.run,
        .sim           = &fx.sim,
        .store         = &store,
    };
    const struct spg_agent_run_config    config = {.max_steps = 5u};
    const struct spg_agent_run_workspace ws     = ws_for(&fx);
    const struct spg_eval_expect         expect = {
        .check_termination = true, .termination = SPG_AGENT_LOOP_FINISHED};
    struct spg_eval_case_result res = {};
    const enum spg_status       s   = spg_eval_run_case(
        sim_finish, 2u, nullptr, &inputs, &config, &ws, &expect, &res);
    /* The rendered context (last step) carries the mind-palace index entry. */
    const int ok = (s == SPG_OK && res.outcome == SPG_EVAL_PASS &&
                    strstr(fx.context, "factslug") != nullptr)
                       ? 0
                       : 1;
    char cmd[128];
    (void)snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    (void)system(cmd);
    return ok;
}

/* #55: the answer-free selector for best-of-N. */
static int test_run_rank(void) {
    /* Strictly ordered, and the order follows the parse/gate/task ladder:
     * a max_steps run PARSED and was ALLOWED and executed real actions; a
     * denied run parsed but never got past the gate. max_steps got further.
     * (#55's issue text had these the other way round — this is the fix.) */
    const int finished = spg_run_rank(SPG_OK, SPG_AGENT_LOOP_FINISHED);
    const int max_steps = spg_run_rank(SPG_OK, SPG_AGENT_LOOP_MAX_STEPS);
    const int budget   = spg_run_rank(SPG_OK, SPG_AGENT_LOOP_BUDGET);
    const int denied   = spg_run_rank(SPG_OK, SPG_AGENT_LOOP_DENIED);
    const int rejected = spg_run_rank(SPG_OK, SPG_AGENT_LOOP_REJECTED);
    const int error    = spg_run_rank(SPG_OK, SPG_AGENT_LOOP_ERROR);

    if (!(finished > max_steps && max_steps > budget && budget > denied &&
          denied > rejected && rejected > error)) {
        fprintf(stderr,
                "rank order: finished=%d max_steps=%d budget=%d denied=%d "
                "rejected=%d error=%d\n",
                finished, max_steps, budget, denied, rejected, error);
        return 1;
    }
    /* A harness failure is never a measurement of the model, whatever the
     * termination says — otherwise a crashed attempt could out-rank a real
     * one and best-of-N would select the broken run. */
    if (spg_run_rank(SPG_E_IO, SPG_AGENT_LOOP_FINISHED) >= rejected ||
        spg_run_rank(SPG_E_MODEL, SPG_AGENT_LOOP_FINISHED) >= rejected) {
        return 1;
    }
    /* Every enumerator is covered: no termination may fall through to the
     * error value by accident. */
    if (error != spg_run_rank(SPG_E_IO, SPG_AGENT_LOOP_FINISHED)) {
        return 1;
    }
    return 0;
}

int main(void) {
    if (test_run_rank() != 0) {
        fprintf(stderr, "test_run_rank failed\n");
        return 1;
    }
    if (test_memory_index_injected() != 0) {
        fprintf(stderr, "test_memory_index_injected failed\n");
        return 1;
    }
    if (test_case_pass() != 0) {
        fprintf(stderr, "test_case_pass failed\n");
        return 1;
    }
    if (test_case_fail_termination() != 0) {
        fprintf(stderr, "test_case_fail_termination failed\n");
        return 1;
    }
    return 0;
}
