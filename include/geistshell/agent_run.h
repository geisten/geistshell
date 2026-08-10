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
 * harness, so both are byte-identical and a production self-improvement loop
 * can call it directly. The runner owns the ephemeral graph/memory; the caller
 * owns the model, journal, store, configs, and the scratch buffers. */

struct spg_agent_run_inputs {
    struct spg_model_adapter       *model;  /* required, initialized */
    const struct spg_policy_config *policy; /* required */
    size_t                          policy_text_n;
    const char                     *policy_text; /* required */
    const struct spg_run_config    *run;         /* required (budgets) */
    struct spg_sim_config          *sim;         /* nullable (no simulator) */
    struct spg_mem_store           *store;       /* nullable (no memory) */
    struct spg_journal_writer *journal; /* nullable (no audit/trajectory) */
    /* Optional concrete worked examples of the recommendation form, rendered
     * in the context's (examples ...) section — few-shot so a small model can
     * imitate the DSL rather than parse the schema grammar. Null = none. */
    const char *exemplars;
    /* Optional free-text task, rendered as (goal "..."). Null = none. */
    const char *goal;
    /* Optional machine telemetry snapshot for the context (roadmap phase 3).
     * Null = none, the default: the context is then byte-identical to before
     * machine state existed, which the phase-0 journal freeze relies on. */
    /* Mutable since phase 7: the loop replaces it between ticks so the tick
     * after an action sees what the action did. */
    struct spg_machine_state *machine;
    /* Process profile for machine actions (roadmap phase 6). Null = every
     * machine action is denied as unmanaged, which is the right default. */
    const struct spg_process_profile *profile;
    /* Ledger of pauses this run owes a resume for (phase 6b, #80). Null means
     * machine pauses are refused: stopping a process nobody promised to
     * restart is the failure mode this exists to prevent. */
    struct spg_machine_pause_ledger *pause_ledger;
    /* Phase 7: the snapshot to install once an action has executed, for
     * scripted cases where the world must change deterministically. Null on a
     * live run, which re-samples the host instead. */
    const struct spg_machine_state *machine_after;
    bool                            refresh_machine;
    /* Milliseconds to let the machine settle before re-observing. See
     * spg_agent_loop_config.machine_settle_ms — without it a live run measures
     * the load from before its own action. */
    uint64_t                        machine_settle_ms;
    /* Optional goal for the context (phase 9). The harness, not the loop,
     * decides whether it was met. */
    const struct spg_machine_goal  *machine_goal;
    /* How to speak to this model (#54). Null = the bare context. */
    const struct spg_model_profile *profile_model;
    /* Phase 11 (#71): parts of the snapshot to withhold, to measure what the
     * model actually needs. 0 = everything, the default. */
    uint32_t                        machine_ablate;
};

struct spg_agent_run_config {
    size_t max_steps;
    size_t max_repairs;
    bool finish_on_no_progress; /* #40: converge -> FINISHED (see agent_loop.h)
                                 */
    bool     execution_enabled;
    uint64_t exec_timeout_ms;
    size_t   exec_stdout_cap;
    size_t   exec_stderr_cap;
    /* exec_working_dir / prefix default to "." when null. */
    const char *exec_working_dir;
    const char *exec_workdir_prefix;
    size_t      context_refs; /* graph/memory/journal-event context limit */
    /* Slug of a stored lesson whose directive is rendered every step as
     * (directive "...") — the strong steering channel (vs the mind-palace
     * index). Null/absent or missing in the store -> no directive line. */
    const char *directive_slug;
};

/* Caller-owned scratch. All buffers must be non-null with non-zero capacity
 * (except shell_* which may be null when execution is never used). */
struct spg_agent_run_workspace {
    size_t                          context_capacity;
    /* Scratch for the chat-framed prompt (#54); null skips framing. */
    size_t                          framed_capacity;
    char                           *framed;
    char                           *context;
    size_t                          model_output_capacity;
    char                           *model_output;
    size_t                          graph_ref_capacity;
    struct spg_context_graph_ref   *graph_refs;
    size_t                          memory_ref_capacity;
    struct spg_context_memory_ref  *memory_refs;
    size_t                          journal_ref_capacity;
    struct spg_context_journal_ref *journal_refs;
    size_t                          token_capacity;
    struct spg_sexpr_token         *tokens;
    size_t                          node_capacity;
    struct spg_sexpr_node          *nodes;
    size_t                          policy_payload_capacity;
    char                           *policy_payload;
    size_t                          sim_payload_capacity;
    char                           *sim_payload;
    size_t                          observation_capacity;
    char  *observation; /* also receives the final observation */
    size_t shell_stdout_capacity;
    char  *shell_stdout;
    size_t shell_stderr_capacity;
    char  *shell_stderr;
    size_t trajectory_capacity;
    struct spg_journal_record_header *trajectory;
    /* Optional: when inputs->store is set, the mind-palace index is rendered
     * here each step and injected into context (so lessons/memories recall). */
    size_t memory_index_capacity;
    char  *memory_index;
};

/* Runs the loop, filling *usage (zeroed first) and *result. Returns
 * SPG_E_INVALID_ARG on missing required inputs; otherwise the loop's status. */
[[nodiscard]] enum spg_status
spg_agent_run(const struct spg_agent_run_inputs    *inputs,
              const struct spg_agent_run_config    *config,
              const struct spg_agent_run_workspace *workspace,
              struct spg_policy_usage              *usage,
              struct spg_agent_loop_result         *result);

#ifdef __cplusplus
}
#endif

#endif
