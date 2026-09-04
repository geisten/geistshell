#ifndef GEISTSHELL_POLICY_GATE_H
#define GEISTSHELL_POLICY_GATE_H

#include "geistshell/device.h"
#include "geistshell/graph.h"
#include "geistshell/journal.h"
#include "geistshell/process_profile.h"
#include "geistshell/policy.h"
#include "geistshell/policy_config.h"
#include "geistshell/recommendation.h"
#include "geistshell/status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct spg_policy_gate_state {
    size_t                          policy_text_n;
    const char                     *policy_text;
    size_t                          recommendation_text_n;
    const char                     *recommendation_text;
    const struct spg_policy_config *policy;
    const struct spg_policy_usage  *usage;

    struct spg_journal_writer *journal;
    struct spg_graph          *graph;

    /* Roadmap phase 6 (#66). A machine action names a profile id; whether it
     * may be performed depends on the profile's role and permissions, and on
     * the process still being where it was observed. Both live HERE rather
     * than in the executor: a safety property enforced downstream of the gate
     * is one the journal cannot show was checked. Null = machine actions are
     * denied outright, which is the right default for a run that never
     * configured a profile. */
    const struct spg_process_profile *profile;
    const struct spg_machine_state   *machine;

    /* #119: the loaded channel table. For a device_write the gate derives the
     * request's network need from the TARGET CHANNEL's operator-declared
     * (network ...) field — never from model text (the recommendation form
     * requires uses_network false, and the parser rejects anything else). So
     * under (network_default deny) a network channel is refused before any
     * fork, while local channels keep working. Null = no device on this run;
     * the executor reports NO_DEVICE downstream and nothing forks. */
    const struct spg_device *device;
};

struct spg_policy_gate_config {
    uint32_t actor_id;
    uint64_t timestamp_ns;
    uint64_t parent_sequence;

    bool write_journal;
    bool update_graph_on_deny;
    bool has_recommendation_node;
    struct spg_node_id recommendation_node;
};

struct spg_policy_gate_workspace {
    size_t payload_capacity;
    char  *payload;
};

struct spg_policy_gate_result {
    struct spg_policy_decision decision;

    uint64_t policy_sequence;

    bool               has_policy_node;
    struct spg_node_id policy_node;
    bool               has_blocked_edge;
    struct spg_edge_id blocked_edge;

    size_t payload_used;
    bool   payload_truncated;
};

[[nodiscard]] enum spg_status
spg_policy_gate_step(const struct spg_policy_gate_state *state,
                     const struct spg_policy_gate_config *config,
                     const struct spg_recommendation *recommendation,
                     const struct spg_policy_gate_workspace *workspace,
                     struct spg_policy_gate_result *result);

#ifdef __cplusplus
}
#endif

#endif
