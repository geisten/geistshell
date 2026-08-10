#ifndef GEISTSHELL_POLICY_H
#define GEISTSHELL_POLICY_H

#include "geistshell/policy_config.h"
#include "geistshell/run_config.h"
#include "geistshell/sexpr.h"
#include "geistshell/status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum spg_action_kind {
    SPG_ACTION_LOCAL_SHELL = 0,
    SPG_ACTION_SSH_AUTH_PROBE,
    SPG_ACTION_SIMULATOR,
    SPG_ACTION_MEMORY_SAVE,
    SPG_ACTION_MEMORY_DELETE,
    SPG_ACTION_MEMORY_READ,
    /* Machine actions (roadmap phase 6, #66). Typed and closed: the model
     * never gets a shell for these. Both are reversible, which is why they are
     * the first two — an irreversible action would need a stronger story than
     * "the policy said yes". */
    SPG_ACTION_MACHINE_PAUSE,
    SPG_ACTION_MACHINE_RESUME,
    /* Control action: the agent declares the task complete. Carries no
     * capability and consumes no budget; the loop terminates on it and it never
     * reaches the policy gate. */
    SPG_ACTION_FINISH,
};

enum spg_policy_decision_kind {
    SPG_POLICY_DECISION_ALLOW = 0,
    SPG_POLICY_DECISION_DENY,
};

enum spg_policy_deny_reason {
    SPG_POLICY_DENY_NONE = 0,
    SPG_POLICY_DENY_UNKNOWN_CAPABILITY,
    SPG_POLICY_DENY_DISABLED_CAPABILITY,
    SPG_POLICY_DENY_KIND_MISMATCH,
    SPG_POLICY_DENY_NETWORK,
    SPG_POLICY_DENY_CAPABILITY_BUDGET,
    SPG_POLICY_DENY_GLOBAL_BUDGET,
    SPG_POLICY_DENY_INVALID_REQUEST,
    /* The target is not in the process profile. An unmanaged process is never
     * actionable — not a no-op, a denial, so the model learns the difference. */
    SPG_POLICY_DENY_UNMANAGED_PROCESS,
    /* The profile manages it and forbids this action (role critical, or
     * may_pause/may_stop false). Checked here rather than in the executor:
     * the prompt cannot argue with the policy layer. */
    SPG_POLICY_DENY_PROCESS_PROTECTED,
    /* The pid no longer belongs to the process that was observed. */
    SPG_POLICY_DENY_PROCESS_IDENTITY,
};

struct spg_action_request {
    enum spg_action_kind kind;
    struct spg_text_span capability;
    bool                uses_network;
    uint64_t            cost;
};

struct spg_policy_usage {
    struct spg_run_budgets consumed;
};

struct spg_policy_decision {
    enum spg_policy_decision_kind kind;
    enum spg_policy_deny_reason   deny_reason;
    size_t                        capability_index;
};

[[nodiscard]] enum spg_status spg_policy_decide(
    size_t policy_text_n, const char policy_text[],
    const struct spg_policy_config *policy, const struct spg_policy_usage *usage,
    const struct spg_action_request *request,
    struct spg_policy_decision *decision);

[[nodiscard]] const char *
spg_policy_deny_reason_to_string(enum spg_policy_deny_reason reason);

/* Canonical lowercase name of an action kind ("local_shell", "memory_save",
 * ...). The single source of truth for the kind->name mapping; returns
 * "unknown" for an out-of-range value. */
[[nodiscard]] const char *spg_action_kind_to_string(enum spg_action_kind kind);

#ifdef __cplusplus
}
#endif

#endif
