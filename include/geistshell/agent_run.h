#ifndef GEISTSHELL_AGENT_RUN_H
#define GEISTSHELL_AGENT_RUN_H

#include "geistshell/agent_loop.h"
#include "geistshell/journal.h"
#include "geistshell/mem_store.h"
#include "geistshell/model_adapter.h"
#include "geistshell/policy_config.h"
#include "geistshell/run_config.h"
#include "geistshell/sim_config.h"
#include "geistshell/status.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One-call governed agent run: assemble the orchestrator state + workspace from
 * loaded configs and a (caller-initialized) model adapter, then drive the
 * governed agent loop to termination. This is the single reusable "run a
 * governed agent" primitive shared by the CLI `agent` command and the eval
 * harness, so both are byte-identical and a production self-improvement loop can
 * call it directly. The runner owns the ephemeral graph/memory; the caller owns
 * the model, journal, store, configs, and the scratch buffers. */

struct spg_agent_run_inputs {
    struct spg_model_adapter       *model;       /* required, initialized */
    const struct spg_policy_config *policy;      /* required */
    size_t                          policy_text_n;
    const char                     *policy_text; /* required */
    const struct spg_run_config    *run;         /* required (budgets) */
    struct spg_sim_config          *sim;         /* nullable (no simulator) */
    struct spg_mem_store           *store;       /* nullable (no memory) */
    struct spg_journal_writer      *journal;     /* nullable (no audit/trajectory) */
    /* Optional concrete worked examples of the recommendation form, rendered
     * in the context's (examples ...) section — few-shot so a small model can
     * imitate the DSL rather than parse the schema grammar. Null = none. */
    const char                     *exemplars;
};

struct spg_agent_run_config {
    size_t   max_steps;
    size_t   max_repairs;
    bool     finish_on_no_progress; /* #40: converge -> FINISHED (see agent_loop.h) */
    bool     execution_enabled;
    uint64_t exec_timeout_ms;
    size_t   exec_stdout_cap;
    size_t   exec_stderr_cap;
    /* exec_working_dir / prefix default to "." when null. */
    const char *exec_working_dir;
    const char *exec_workdir_prefix;
    size_t      context_refs; /* graph/memory/journal-event context limit */
};

/* Caller-owned scratch. All buffers must be non-null with non-zero capacity
 * (except shell_* which may be null when execution is never used). */
struct spg_agent_run_workspace {
    size_t                  context_capacity;
    char                   *context;
    size_t                  model_output_capacity;
    char                   *model_output;
    size_t                  graph_ref_capacity;
    struct spg_context_graph_ref *graph_refs;
    size_t                  memory_ref_capacity;
    struct spg_context_memory_ref *memory_refs;
    size_t                  journal_ref_capacity;
    struct spg_context_journal_ref *journal_refs;
    size_t                  token_capacity;
    struct spg_sexpr_token *tokens;
    size_t                  node_capacity;
    struct spg_sexpr_node  *nodes;
    size_t                  policy_payload_capacity;
    char                   *policy_payload;
    size_t                  sim_payload_capacity;
    char                   *sim_payload;
    size_t                  observation_capacity;
    char                   *observation; /* also receives the final observation */
    size_t                  shell_stdout_capacity;
    char                   *shell_stdout;
    size_t                  shell_stderr_capacity;
    char                   *shell_stderr;
    size_t                  trajectory_capacity;
    struct spg_journal_record_header *trajectory;
    /* Optional: when inputs->store is set, the mind-palace index is rendered
     * here each step and injected into context (so lessons/memories recall). */
    size_t                  memory_index_capacity;
    char                   *memory_index;
};

/* Runs the loop, filling *usage (zeroed first) and *result. Returns
 * SPG_E_INVALID_ARG on missing required inputs; otherwise the loop's status. */
[[nodiscard]] enum spg_status
spg_agent_run(const struct spg_agent_run_inputs *inputs,
              const struct spg_agent_run_config *config,
              const struct spg_agent_run_workspace *workspace,
              struct spg_policy_usage *usage,
              struct spg_agent_loop_result *result);

#ifdef __cplusplus
}
#endif

#endif
