#ifndef GEISTSHELL_CONTEXT_H
#define GEISTSHELL_CONTEXT_H

#include "geistshell/device.h"
#include "geistshell/graph.h"
#include "geistshell/journal.h"
#include "geistshell/machine_goal.h"
#include "geistshell/machine_state.h"
#include "geistshell/memory.h"
#include "geistshell/policy.h"
#include "geistshell/run_config.h"
#include "geistshell/status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct spg_context_limits {
    size_t graph_nodes;
    size_t memory_facts;
    size_t journal_events;
};

struct spg_context_sources {
    const struct spg_run_config            *run;
    const struct spg_policy_usage          *usage;
    const struct spg_graph                 *graph;
    const struct spg_memory                *memory;
    size_t                                  journal_header_count;
    const struct spg_journal_record_header *journal_headers;
    size_t                                  graph_text_n;
    const char                             *graph_text;
    size_t                                  memory_text_n;
    const char                             *memory_text;
    /* Pre-rendered long-term memory index (one hook per line), or null. */
    const char *memory_index;
    /* Content of the most recently recalled memory (memory_read), or null. */
    const char *observation;
    /* Optional concrete worked examples of the recommendation form, rendered
     * right after the (contract ...) schema. The schema is a grammar; a small
     * model imitates filled-in examples far more reliably than it parses a
     * grammar. Null = none (unchanged behaviour). */
    const char *exemplars;
    /* Optional free-text task, rendered near the top as (goal "..."). Null =
     * none: the scenario graph is the whole task. */
    const char *goal;
    /* #79: bounded history window, rendered as (machine-history ...) right
     * before the current snapshot — the trend, then the now. Null or a
     * disabled (window 0) history renders nothing, byte-identical to a
     * context without the feature. */
    const struct spg_machine_history *machine_history;
    /* Optional machine telemetry snapshot, rendered as (machine-state ...).
     * Null = none, which is the default and keeps the context byte-identical
     * to before this existed — the phase-0 journal freeze depends on that. */
    const struct spg_machine_state *machine;
    /* Optional goal, rendered as (machine-goal ...) right before the state so
     * the model reads what the run is FOR before what it is looking at. Null =
     * no goal, the default, and the context is unchanged. */
    const struct spg_machine_goal *machine_goal;
    /* Which parts of the snapshot to leave out (phase 11, #71). 0 = the full
     * block, which is the default and byte-identical to before. */
    uint32_t machine_ablate;
    /* Optional plant readings, rendered as (device-state ...) right after the
     * machine block: the host first, then the thing the host is driving. Null
     * = none, which is the default and leaves the context byte-identical to
     * before this existed — the frozen journals depend on that.
     *
     * This is perception, not an action: the agent is GIVEN the readings and
     * never spends a step asking for them. A control loop that burns a tick to
     * measure runs at half the frequency, and a read has nothing for the
     * policy gate to decide. */
    const struct spg_device_state *device_state;
    /* Optional standing directive (a learned lesson) rendered prominently as
     * (directive "...") every step — a stronger channel than the mind-palace
     * index for steering a small model's behaviour (geistshell#40 follow-up).
     * Null = none. */
    const char *directive;
    /* Optional user-profile line (geistshell#28), pre-rendered as one
     * `(profile "...")` s-expression. Shapes framing/defaults only — it is
     * context, never consulted by the policy gate — and is budgeted to a
     * single line so a growing profile never grows the window. Null = none,
     * the default, byte-identical to before. */
    const char *user_profile;
    /* Pre-rendered command menu (#56), one line per command, placed in the
     * CONSTANT part of the context so it is pinnable with the contract (#58).
     * Null = the model is told about no commands, which was this table's state
     * for its whole life: it existed in the tree and was never rendered.
     *
     * A proposal space, not an allowlist — see cmd_menu.h. */
    const char *tools;
    /* Optional live host telemetry, pre-rendered as one `(host_status ...)`
     * line (CPU count, load average, temperature, process count). Null = none,
     * which is the default: it costs context budget on every single step, so a
     * run that never reasons about the machine should not pay for it. */
    const char *host_status;
};

struct spg_context_budget_item {
    uint64_t configured;
    uint64_t consumed;
    uint64_t remaining;
};

struct spg_context_budget_view {
    struct spg_context_budget_item inference_steps;
    struct spg_context_budget_item tokens;
    struct spg_context_budget_item shell_actions;
    struct spg_context_budget_item sim_actions;
    struct spg_context_budget_item memory_actions;
    struct spg_context_budget_item wall_ms;
};

struct spg_context_graph_ref {
    struct spg_node_id node;
    uint32_t           rank;
};

struct spg_context_memory_ref {
    struct spg_fact_id fact;
    uint32_t           rank;
};

struct spg_context_journal_ref {
    uint64_t sequence;
    uint64_t parent_sequence;
    uint64_t payload_bytes;
    uint32_t event_kind;
    uint32_t status;
    size_t   source_index;
};

struct spg_context_view {
    struct spg_context_budget_view budgets;

    size_t                        graph_ref_count;
    size_t                        graph_ref_capacity;
    struct spg_context_graph_ref *graph_refs;
    bool                          graph_truncated;

    size_t                         memory_ref_count;
    size_t                         memory_ref_capacity;
    struct spg_context_memory_ref *memory_refs;
    bool                           memory_truncated;

    size_t                          journal_ref_count;
    size_t                          journal_ref_capacity;
    struct spg_context_journal_ref *journal_refs;
    bool                            journal_truncated;
};

void spg_context_view_init(
    struct spg_context_view *view, size_t graph_ref_capacity,
    struct spg_context_graph_ref   graph_refs[static graph_ref_capacity],
    size_t                         memory_ref_capacity,
    struct spg_context_memory_ref  memory_refs[static memory_ref_capacity],
    size_t                         journal_ref_capacity,
    struct spg_context_journal_ref journal_refs[static journal_ref_capacity]);

[[nodiscard]] enum spg_status
spg_context_build(const struct spg_context_sources *sources,
                  const struct spg_context_limits  *limits,
                  struct spg_context_view          *view);

/* out_prefix_len (nullable, #58) receives the byte length of the CONSTANT
 * prefix — contract + directive + goal + tools + examples, everything before
 * the first per-tick block — so a caller can pin exactly that many prompt
 * tokens into the KV cache across a run. It is a byte offset into dst; a pin
 * layer must still verify token alignment (bytes are not tokens). */
[[nodiscard]] enum spg_status
spg_context_render(const struct spg_context_sources *sources,
                   const struct spg_context_view *view, size_t dst_capacity,
                   char dst[static dst_capacity], size_t *out_required,
                   size_t *out_prefix_len);

#ifdef __cplusplus
}
#endif

#endif
