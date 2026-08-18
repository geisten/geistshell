#ifndef GEISTSHELL_RUN_CONFIG_H
#define GEISTSHELL_RUN_CONFIG_H

#include "geistshell/sexpr.h"
#include "geistshell/status.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct spg_run_budgets {
    uint64_t inference_steps;
    uint64_t tokens;
    uint64_t shell_actions;
    uint64_t sim_actions;
    uint64_t memory_actions;  /* optional in config; defaults to unlimited */
    uint64_t machine_actions; /* optional in config; defaults to unlimited */
    uint64_t wall_ms;         /* enforced between steps by the agent loop */
};

struct spg_run_config {
    struct spg_text_span model_path;
    struct spg_text_span policy_path;
    struct spg_text_span scenario_path;
    struct spg_text_span corpus_manifest_path;
    struct spg_text_span journal_path;

    uint64_t               seed;
    struct spg_run_budgets budgets;

    /* Optional success criterion for learning from real runs (docs/LEARNING.md,
     * P1). When has_expect, the run is judged: it succeeded iff it finished and
     * its observation contains expect_observation (a substring into `input`).
     * The shell observation folds in the exit code, so a marker substring
     * verifies real outcomes. Absent -> no success-side verdict; the run's
     * terminal failures still teach through the existing path. Model-free,
     * zero tokens: the world supplies the ground truth. */
    bool                 has_expect;
    struct spg_text_span expect_observation;

    /* Optional free-text task the agent is to accomplish, rendered into the
     * context as (goal "..."). geistshell's task is otherwise only the scenario;
     * a goal lets a run state an objective the model decides how to meet — the
     * headroom a learning lesson can shape. Absent -> no goal line. */
    bool                 has_goal;
    struct spg_text_span goal;
};

struct spg_run_config_error {
    enum spg_status status;
    uint32_t        node_index;
    size_t          offset;
};

/* Shared budget-list parser used by both the run and policy config loaders.
 * budgets_field is the `(budgets ...)` field node (e.g. from find_field); its
 * value list is parsed into *out, which must name every budget exactly once.
 * On failure the status is returned and the offending node is reported via
 * *err_node / *err_offset so each loader can map it onto its own error struct.
 * On success *out is fully populated; on failure *out is left zero-initialized. */
[[nodiscard]] enum spg_status spg_run_budgets_parse(
    size_t input_n, const char input[],
    const struct spg_sexpr_node nodes[static 1], uint32_t budgets_field,
    struct spg_run_budgets *out, uint32_t *err_node, size_t *err_offset);

[[nodiscard]] enum spg_status spg_run_config_load(
    size_t input_n, const char input[], size_t token_capacity,
    struct spg_sexpr_token tokens[static token_capacity], size_t node_capacity,
    struct spg_sexpr_node nodes[static node_capacity],
    struct spg_run_config *out, struct spg_run_config_error *error);

#ifdef __cplusplus
}
#endif

#endif
