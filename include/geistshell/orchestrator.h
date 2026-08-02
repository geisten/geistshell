#ifndef GEISTSHELL_ORCHESTRATOR_H
#define GEISTSHELL_ORCHESTRATOR_H

#include "geistshell/actor.h"
#include "geistshell/mem_executor.h"
#include "geistshell/policy_gate.h"
#include "geistshell/recommendation.h"
#include "geistshell/shell_executor.h"
#include "geistshell/sim_executor.h"
#include "geistshell/status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum spg_orchestrator_stage {
    SPG_ORCHESTRATOR_STAGE_NOT_STARTED = 0,
    SPG_ORCHESTRATOR_STAGE_ACTOR_DONE,
    SPG_ORCHESTRATOR_STAGE_RECOMMENDATION_REJECTED,
    /* The agent emitted a `finish` control action: a valid recommendation that
     * short-circuits before the policy gate (no capability, no side effect).
     * Ordered below POLICY_GATED so policy_evaluated() stays false for it. */
    SPG_ORCHESTRATOR_STAGE_FINISHED,
    SPG_ORCHESTRATOR_STAGE_POLICY_GATED,
    SPG_ORCHESTRATOR_STAGE_SIM_EXECUTED,
    SPG_ORCHESTRATOR_STAGE_MEMORY_EXECUTED,
    SPG_ORCHESTRATOR_STAGE_SHELL_EXECUTED,
};

struct spg_orchestrator_state {
    struct spg_graph          *graph;
    struct spg_memory         *memory;
    struct spg_journal_writer *journal;
    struct spg_model_adapter  *model;
    struct spg_sim_config     *sim;
    struct spg_mem_store      *store;

    const struct spg_run_config    *run;
    const struct spg_policy_usage  *usage;
    const struct spg_policy_config *policy;

    size_t      policy_text_n;
    const char *policy_text;

    size_t                                  journal_header_count;
    const struct spg_journal_record_header *journal_headers;

    size_t      graph_text_n;
    const char *graph_text;
    size_t      memory_text_n;
    const char *memory_text;
    const char *memory_index;
    const char *observation;
    /* Optional concrete worked examples of the recommendation form (context
     * (examples ...) section) — few-shot for a small model. Null = none. */
    const char *exemplars;
};

struct spg_orchestrator_config {
    uint32_t actor_id;
    uint64_t timestamp_ns;
    uint64_t parent_sequence;

    struct spg_context_limits context_limits;
    size_t                    max_decode_tokens;

    bool reset_model_session;
    bool write_journal;
    bool update_graph;
    bool update_memory;

    /* Governed local_shell execution. When a local_shell action is ALLOW'd, the
     * shell executor runs it (gated by the boundary) inside working_dir; with
     * execution_enabled false the boundary denies and only records the attempt. */
    bool        execution_enabled;
    const char *exec_working_dir;
    const char *exec_workdir_prefix;
    uint64_t    exec_timeout_ms;
    size_t      exec_stdout_cap;
    size_t      exec_stderr_cap;
};

struct spg_orchestrator_workspace {
    struct spg_actor_step_workspace actor;

    size_t                  recommendation_token_capacity;
    struct spg_sexpr_token *recommendation_tokens;
    size_t                  recommendation_node_capacity;
    struct spg_sexpr_node  *recommendation_nodes;

    size_t policy_payload_capacity;
    char  *policy_payload;

    size_t sim_payload_capacity;
    char  *sim_payload;

    /* The shared "last observation" channel: memory_read content OR local_shell
     * output is written here and rendered into the next step's context. */
    size_t observation_capacity;
    char  *observation_buf;

    /* Scratch for local_shell stdout/stderr capture. */
    size_t shell_stdout_capacity;
    char  *shell_stdout_buf;
    size_t shell_stderr_capacity;
    char  *shell_stderr_buf;

    /* Optional: when a store is present, the tick re-renders the mind-palace
     * index into this buffer and injects it into context, so saved memories and
     * lessons are recalled. Null falls back to state->memory_index. */
    size_t memory_index_capacity;
    char  *memory_index_buf;
};

struct spg_orchestrator_result {
    enum spg_orchestrator_stage stage;

    struct spg_actor_step_result      actor;
    struct spg_recommendation        recommendation;
    struct spg_recommendation_error  recommendation_error;
    struct spg_policy_gate_result    policy_gate;
    struct spg_sim_executor_result   sim;
    struct spg_mem_executor_result   memory;
    struct spg_shell_executor_result shell;
};

[[nodiscard]] enum spg_status
spg_orchestrator_tick(struct spg_orchestrator_state *state,
                      const struct spg_orchestrator_config *config,
                      const struct spg_orchestrator_workspace *workspace,
                      struct spg_orchestrator_result *result);

[[nodiscard]] const char *
spg_orchestrator_stage_to_string(enum spg_orchestrator_stage stage);

/* Derived views of a tick result. Each is a pure function of `stage`, the
 * single source of truth for how far the tick advanced. */

/* The model produced a structurally valid recommendation (the tick advanced
 * past the actor/reject stages — i.e. it was gated, executed, or `finish`). */
[[nodiscard]] bool spg_orchestrator_recommendation_valid(
    const struct spg_orchestrator_result *result);

/* The policy gate ran (the recommendation was a gateable action, not `finish`). */
[[nodiscard]] bool spg_orchestrator_policy_evaluated(
    const struct spg_orchestrator_result *result);

/* The simulator executed this tick. */
[[nodiscard]] bool
spg_orchestrator_sim_executed(const struct spg_orchestrator_result *result);

/* A memory action executed this tick. */
[[nodiscard]] bool
spg_orchestrator_memory_executed(const struct spg_orchestrator_result *result);

/* A local_shell action executed this tick (ran or was boundary-denied). */
[[nodiscard]] bool
spg_orchestrator_shell_executed(const struct spg_orchestrator_result *result);

/* The agent emitted `finish`: the loop should terminate as task-complete. */
[[nodiscard]] bool
spg_orchestrator_finished(const struct spg_orchestrator_result *result);

#ifdef __cplusplus
}
#endif

#endif
