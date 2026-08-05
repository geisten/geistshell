#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#    define _DARWIN_C_SOURCE 1
#endif

#include "geistshell/agent_loop.h"

#include "geistshell/graph.h"
#include "geistshell/mem_store.h"
#include "geistshell/memory.h"
#include "geistshell/model_adapter.h"
#include "geistshell/policy_config.h"
#include "geistshell/sim_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char policy_text[] =
    "(policy"
    " (network_default deny)"
    " (budgets"
    "  (inference_steps 100)"
    "  (tokens 4096)"
    "  (shell_actions 8)"
    "  (sim_actions 8)"
    "  (wall_ms 60000)"
    "  (journal_bytes 1048576)"
    "  (risk_bp 10000))"
    " (capability"
    "  ((name sim.act) (kind simulator) (enabled true) (budget 8))"
    "  ((name build.run) (kind local_shell) (enabled true) (budget 8))"
    "  ((name mem.write) (kind memory) (enabled true) (budget 8))))";

static const char scenario_text[] =
    "(scenario"
    " (host (id web) (criticality_bp 8000))"
    " (service (id ssh_web) (host web) (name ssh) (port 22) (exposure_bp 5000))"
    " (vulnerability (id cve) (service ssh_web) (severity_bp 8000) (patched "
    "false)))";

#define FR(s) ((struct spg_fake_response){.n = sizeof(s) - 1u, .text = (s)})

struct loop_fixture {
    struct spg_policy_config  policy;
    struct spg_policy_usage   usage;
    struct spg_sim_config     sim;
    struct spg_graph          graph;
    struct spg_memory         memory;
    struct spg_mem_store      store;
    struct spg_journal_writer journal;
    struct spg_model_adapter  model;
    bool                      have_journal;
    char                      journal_path[64];
    char                      store_dir[64];

    struct spg_context_graph_ref   graph_refs[8];
    struct spg_context_memory_ref  memory_refs[8];
    struct spg_context_journal_ref journal_refs[8];
    char                           context[8192];
    char                           model_output[1024];
    struct spg_sexpr_token         rec_tokens[128];
    struct spg_sexpr_node          rec_nodes[128];
    char                           policy_payload[1024];
    char                           sim_payload[1024];
    char                           observation[4096];
    char                           shell_stdout[4096];
    char                           shell_stderr[1024];
    struct spg_journal_record_header trajectory[256];
};

static int load_policy(struct spg_policy_config *policy) {
    struct spg_sexpr_token         tokens[192];
    struct spg_sexpr_node          nodes[192];
    struct spg_policy_config_error error = {};
    return spg_policy_config_load(strlen(policy_text), policy_text, 192u, tokens,
                                  192u, nodes, policy, &error) == SPG_OK
               ? 0
               : 1;
}

static int load_sim(struct spg_sim_config *sim) {
    struct spg_sexpr_token      tokens[256];
    struct spg_sexpr_node       nodes[256];
    struct spg_sim_config_error error = {};
    return spg_sim_config_load(strlen(scenario_text), scenario_text, 256u,
                               tokens, 256u, nodes, sim, &error) == SPG_OK
               ? 0
               : 1;
}

/* Run a scripted agent loop. Returns 0 on setup success (the loop's own status
 * is in *out via the result + the returned run status). */
static int run_script(const struct spg_fake_response *resp, const size_t count,
                      const size_t max_steps, const bool exec_enabled,
                      const size_t max_repairs, const uint64_t step_budget,
                      const bool finish_on_no_progress,
                      struct loop_fixture *fx,
                      struct spg_agent_loop_result *out) {
    *fx = (struct loop_fixture){};
    spg_graph_init(&fx->graph);
    spg_memory_init(&fx->memory);

    memcpy(fx->journal_path, "/tmp/spg_loop_XXXXXX", 21u);
    if (mkstemp(fx->journal_path) < 0) {
        return 1;
    }
    memcpy(fx->store_dir, "/tmp/spg_loopmem_XXXXXX", 24u);
    if (mkdtemp(fx->store_dir) == nullptr) {
        return 1;
    }

    const struct spg_model_adapter_config model_config = {
        .kind                = SPG_MODEL_ADAPTER_FAKE,
        .sampling            = {.top_p = 1.0f},
        .fake_response_count = count,
        .fake_responses      = resp,
    };
    if (load_policy(&fx->policy) != 0 || load_sim(&fx->sim) != 0 ||
        spg_mem_store_open(&fx->store, fx->store_dir) != SPG_OK ||
        spg_model_adapter_init(&fx->model, &model_config) != SPG_OK ||
        spg_journal_writer_open(&fx->journal, fx->journal_path) != SPG_OK) {
        return 1;
    }
    fx->have_journal = true;

    const struct spg_orchestrator_workspace workspace = {
        .actor = {.context_capacity      = sizeof fx->context,
                  .context               = fx->context,
                  .model_output_capacity = sizeof fx->model_output,
                  .model_output          = fx->model_output,
                  .graph_ref_capacity    = 8u,
                  .graph_refs            = fx->graph_refs,
                  .memory_ref_capacity   = 8u,
                  .memory_refs           = fx->memory_refs,
                  .journal_ref_capacity  = 8u,
                  .journal_refs          = fx->journal_refs},
        .recommendation_token_capacity = 128u,
        .recommendation_tokens         = fx->rec_tokens,
        .recommendation_node_capacity  = 128u,
        .recommendation_nodes          = fx->rec_nodes,
        .policy_payload_capacity       = sizeof fx->policy_payload,
        .policy_payload                = fx->policy_payload,
        .sim_payload_capacity          = sizeof fx->sim_payload,
        .sim_payload                   = fx->sim_payload,
        .observation_capacity        = sizeof fx->observation,
        .observation_buf             = fx->observation,
        .shell_stdout_capacity         = sizeof fx->shell_stdout,
        .shell_stdout_buf              = fx->shell_stdout,
        .shell_stderr_capacity         = sizeof fx->shell_stderr,
        .shell_stderr_buf              = fx->shell_stderr,
    };

    struct spg_orchestrator_state state = {
        .graph         = &fx->graph,
        .memory        = &fx->memory,
        .journal       = &fx->journal,
        .model         = &fx->model,
        .sim           = &fx->sim,
        .store         = &fx->store,
        .usage         = &fx->usage,
        .policy        = &fx->policy,
        .policy_text_n = strlen(policy_text),
        .policy_text   = policy_text,
        .observation = fx->observation,
    };
    const struct spg_agent_loop_config loop_config = {
        .base = {.actor_id          = 1u,
                 .context_limits    = {.graph_nodes    = 8u,
                                       .memory_facts   = 8u,
                                       .journal_events = 8u},
                 .max_decode_tokens = 2u,
                 .write_journal     = true,
                 .update_graph      = true,
                 .update_memory     = true,
                 .execution_enabled = exec_enabled,
                 .exec_working_dir  = ".",
                 .exec_workdir_prefix = ".",
                 .exec_timeout_ms     = 5000u,
                 .exec_stdout_cap     = sizeof fx->shell_stdout,
                 .exec_stderr_cap     = sizeof fx->shell_stderr},
        .max_steps               = max_steps,
        .max_repairs             = max_repairs,
        .step_budget             = step_budget,
        .finish_on_no_progress   = finish_on_no_progress,
        .journal_header_capacity = sizeof fx->trajectory / sizeof fx->trajectory[0],
        .journal_headers         = fx->trajectory,
    };

    (void)spg_agent_loop_run(&state, &loop_config, &workspace, &fx->usage, out);
    return 0;
}

static void teardown(struct loop_fixture *fx) {
    if (fx->have_journal) {
        (void)spg_journal_writer_close(&fx->journal);
    }
    spg_model_adapter_destroy(&fx->model);
    (void)remove(fx->journal_path);
    /* best-effort store cleanup */
    char cmd[160];
    (void)snprintf(cmd, sizeof cmd, "rm -rf '%s'", fx->store_dir);
    (void)system(cmd);
}

static int test_finish_terminates(void) {
    static const struct spg_fake_response script[] = {
        FR("(recommend (kind memory_save) (capability \"mem.write\") (cost 1) "
           "(uses_network false) (confidence_bp 5000) (reason \"save\") "
           "(slug \"note\") (description \"a note\") (body \"hello\"))"),
        FR("(recommend (kind local_shell) (capability \"build.run\") (cost 1) "
           "(uses_network false) (confidence_bp 5000) (reason \"probe\") "
           "(command \"echo loop-step-ok\"))"),
        FR("(recommend (kind finish) (reason \"done\"))"),
    };
    struct loop_fixture          fx = {};
    struct spg_agent_loop_result out = {};
    if (run_script(script, 3u, 5u, true, 0u, 0u, false, &fx, &out) != 0) {
        teardown(&fx);
        return 1;
    }
    const int ok = (out.termination == SPG_AGENT_LOOP_FINISHED &&
                    out.steps_taken == 3u &&
                    strstr(fx.observation, "loop-step-ok") != nullptr)
                       ? 0
                       : 1;
    teardown(&fx);
    return ok;
}

static int test_max_steps(void) {
    static const struct spg_fake_response script[] = {
        FR("(recommend (kind simulator) (capability \"sim.act\") (cost 1) "
           "(uses_network false) (confidence_bp 7000) (reason \"r\"))"),
        FR("(recommend (kind simulator) (capability \"sim.act\") (cost 1) "
           "(uses_network false) (confidence_bp 7000) (reason \"r\"))"),
    };
    struct loop_fixture          fx = {};
    struct spg_agent_loop_result out = {};
    if (run_script(script, 2u, 2u, false, 0u, 0u, false, &fx, &out) != 0) {
        teardown(&fx);
        return 1;
    }
    const int ok = (out.termination == SPG_AGENT_LOOP_MAX_STEPS &&
                    out.steps_taken == 2u)
                       ? 0
                       : 1;
    teardown(&fx);
    return ok;
}

static int test_no_progress_finishes(void) {
    /* #40: the same local_shell command every step, so each step's observation
     * (the command's stdout) is byte-identical. With finish_on_no_progress the
     * loop treats the repeated observation as convergence and terminates
     * FINISHED at step 2 — the model acts validly but never says finish, yet the
     * run still ends cleanly. Without the flag the same script runs to the cap. */
    static const struct spg_fake_response script[] = {
        FR("(recommend (kind local_shell) (capability \"build.run\") (cost 1) "
           "(uses_network false) (confidence_bp 7000) (reason \"probe\") "
           "(command \"echo converge-marker\"))"),
        FR("(recommend (kind local_shell) (capability \"build.run\") (cost 1) "
           "(uses_network false) (confidence_bp 7000) (reason \"probe\") "
           "(command \"echo converge-marker\"))"),
        FR("(recommend (kind local_shell) (capability \"build.run\") (cost 1) "
           "(uses_network false) (confidence_bp 7000) (reason \"probe\") "
           "(command \"echo converge-marker\"))"),
    };
    struct loop_fixture          fx  = {};
    struct spg_agent_loop_result out = {};
    if (run_script(script, 3u, 5u, true, 0u, 0u, true, &fx, &out) != 0) {
        teardown(&fx);
        return 1;
    }
    const int ok = (out.termination == SPG_AGENT_LOOP_FINISHED &&
                    out.steps_taken == 2u &&
                    strstr(fx.observation, "converge-marker") != nullptr)
                       ? 0
                       : 1;
    teardown(&fx);
    return ok;
}

static int test_step_budget(void) {
    /* A generous max_steps but a step_budget of 2: the loop stops on the policy
     * budget before it would reach `finish`. */
    static const struct spg_fake_response script[] = {
        FR("(recommend (kind simulator) (capability \"sim.act\") (cost 1) "
           "(uses_network false) (confidence_bp 7000) (reason \"r\"))"),
        FR("(recommend (kind simulator) (capability \"sim.act\") (cost 1) "
           "(uses_network false) (confidence_bp 7000) (reason \"r\"))"),
        FR("(recommend (kind finish) (reason \"done\"))"),
    };
    struct loop_fixture          fx = {};
    struct spg_agent_loop_result out = {};
    if (run_script(script, 3u, 10u, false, 0u, 2u, false, &fx, &out) != 0) {
        teardown(&fx);
        return 1;
    }
    const int ok = (out.termination == SPG_AGENT_LOOP_BUDGET &&
                    out.steps_taken == 2u)
                       ? 0
                       : 1;
    teardown(&fx);
    return ok;
}

static int test_denied(void) {
    static const struct spg_fake_response script[] = {
        FR("(recommend (kind simulator) (capability \"nope\") (cost 1) "
           "(uses_network false) (confidence_bp 7000) (reason \"r\"))"),
    };
    struct loop_fixture          fx = {};
    struct spg_agent_loop_result out = {};
    if (run_script(script, 1u, 5u, false, 0u, 0u, false, &fx, &out) != 0) {
        teardown(&fx);
        return 1;
    }
    const int ok =
        (out.termination == SPG_AGENT_LOOP_DENIED && out.steps_taken == 1u)
            ? 0
            : 1;
    teardown(&fx);
    return ok;
}

static int test_rejected(void) {
    static const struct spg_fake_response script[] = {
        FR("not a recommendation at all"),
    };
    struct loop_fixture          fx = {};
    struct spg_agent_loop_result out = {};
    if (run_script(script, 1u, 5u, false, 0u, 0u, false, &fx, &out) != 0) {
        teardown(&fx);
        return 1;
    }
    const int ok =
        (out.termination == SPG_AGENT_LOOP_REJECTED && out.steps_taken == 1u)
            ? 0
            : 1;
    teardown(&fx);
    return ok;
}

static int test_trajectory_feedback(void) {
    /* A first sim step is journaled; the second step's rendered context must
     * carry that event back as recent_events (closed-loop trajectory). */
    static const struct spg_fake_response script[] = {
        FR("(recommend (kind simulator) (capability \"sim.act\") (cost 1) "
           "(uses_network false) (confidence_bp 7000) (reason \"r\"))"),
        FR("(recommend (kind finish) (reason \"done\"))"),
    };
    struct loop_fixture          fx = {};
    struct spg_agent_loop_result out = {};
    if (run_script(script, 2u, 5u, false, 0u, 0u, false, &fx, &out) != 0) {
        teardown(&fx);
        return 1;
    }
    /* fx.context holds the final (finish) step's prompt, which must include the
     * first step's journaled event(s). */
    const int ok = (out.termination == SPG_AGENT_LOOP_FINISHED &&
                    out.steps_taken == 2u &&
                    strstr(fx.context, "(event (sequence ") != nullptr)
                       ? 0
                       : 1;
    teardown(&fx);
    return ok;
}

static int test_self_repair(void) {
    /* A malformed first reply is repaired (error fed back) rather than ending
     * the run; the second, valid reply finishes the task. */
    static const struct spg_fake_response script[] = {
        FR("this is not a recommendation"),
        FR("(recommend (kind finish) (reason \"done\"))"),
    };
    struct loop_fixture          fx = {};
    struct spg_agent_loop_result out = {};
    if (run_script(script, 2u, 5u, false, 1u, 0u, false, &fx, &out) != 0) {
        teardown(&fx);
        return 1;
    }
    const int ok = (out.termination == SPG_AGENT_LOOP_FINISHED &&
                    out.steps_taken == 2u && out.repairs_used == 1u &&
                    strstr(fx.context, "invalid recommendation") != nullptr)
                       ? 0
                       : 1;
    teardown(&fx);
    return ok;
}

static int test_repair_budget_exhausted(void) {
    /* With no repair budget, the first malformed reply terminates the run. */
    static const struct spg_fake_response script[] = {
        FR("garbage"),
        FR("(recommend (kind finish) (reason \"done\"))"),
    };
    struct loop_fixture          fx = {};
    struct spg_agent_loop_result out = {};
    if (run_script(script, 2u, 5u, false, 0u, 0u, false, &fx, &out) != 0) {
        teardown(&fx);
        return 1;
    }
    const int ok = (out.termination == SPG_AGENT_LOOP_REJECTED &&
                    out.steps_taken == 1u && out.repairs_used == 0u)
                       ? 0
                       : 1;
    teardown(&fx);
    return ok;
}

int main(void) {
    if (test_finish_terminates() != 0) {
        fprintf(stderr, "test_finish_terminates failed\n");
        return 1;
    }
    if (test_self_repair() != 0) {
        fprintf(stderr, "test_self_repair failed\n");
        return 1;
    }
    if (test_repair_budget_exhausted() != 0) {
        fprintf(stderr, "test_repair_budget_exhausted failed\n");
        return 1;
    }
    if (test_trajectory_feedback() != 0) {
        fprintf(stderr, "test_trajectory_feedback failed\n");
        return 1;
    }
    if (test_step_budget() != 0) {
        fprintf(stderr, "test_step_budget failed\n");
        return 1;
    }
    if (test_max_steps() != 0) {
        fprintf(stderr, "test_max_steps failed\n");
        return 1;
    }
    if (test_no_progress_finishes() != 0) {
        fprintf(stderr, "test_no_progress_finishes failed\n");
        return 1;
    }
    if (test_denied() != 0) {
        fprintf(stderr, "test_denied failed\n");
        return 1;
    }
    if (test_rejected() != 0) {
        fprintf(stderr, "test_rejected failed\n");
        return 1;
    }
    return 0;
}
