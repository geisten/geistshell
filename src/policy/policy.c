#include "geistshell/policy.h"

#include <string.h>

static bool span_eq(const size_t input_n, const char input[],
                    const struct spg_text_span a,
                    const struct spg_text_span b) {
    if (input == nullptr || !spg_sexpr_span_valid(input_n, a) || !spg_sexpr_span_valid(input_n, b) ||
        a.length != b.length) {
        return false;
    }
    return memcmp(input + a.offset, input + b.offset, a.length) == 0;
}

static void deny(struct spg_policy_decision *decision,
                 const enum spg_policy_deny_reason reason) {
    decision->kind             = SPG_POLICY_DECISION_DENY;
    decision->deny_reason      = reason;
    decision->capability_index = SIZE_MAX;
}

static void allow(struct spg_policy_decision *decision, const size_t cap_index) {
    decision->kind             = SPG_POLICY_DECISION_ALLOW;
    decision->deny_reason      = SPG_POLICY_DENY_NONE;
    decision->capability_index = cap_index;
}

static enum spg_policy_capability_kind
cap_kind_for_action(const enum spg_action_kind kind) {
    switch (kind) {
    case SPG_ACTION_LOCAL_SHELL:
        return SPG_POLICY_CAP_LOCAL_SHELL;
    case SPG_ACTION_SSH_AUTH_PROBE:
        return SPG_POLICY_CAP_SSH_AUTH_PROBE;
    case SPG_ACTION_SIMULATOR:
        return SPG_POLICY_CAP_SIMULATOR;
    case SPG_ACTION_MEMORY_SAVE:
    case SPG_ACTION_MEMORY_DELETE:
    case SPG_ACTION_MEMORY_READ:
        return SPG_POLICY_CAP_MEMORY;
    case SPG_ACTION_MACHINE_PAUSE:
    case SPG_ACTION_MACHINE_RESUME:
        return SPG_POLICY_CAP_MACHINE_PROCESS;
    case SPG_ACTION_MACHINE_FAN:
        return SPG_POLICY_CAP_MACHINE_THERMAL;
    default:
        return SPG_POLICY_CAP_LOCAL_SHELL;
    }
}

static bool action_kind_valid(const enum spg_action_kind kind) {
    return kind == SPG_ACTION_LOCAL_SHELL || kind == SPG_ACTION_SSH_AUTH_PROBE ||
           kind == SPG_ACTION_SIMULATOR || kind == SPG_ACTION_MEMORY_SAVE ||
           kind == SPG_ACTION_MEMORY_DELETE || kind == SPG_ACTION_MEMORY_READ ||
           kind == SPG_ACTION_MACHINE_PAUSE ||
           kind == SPG_ACTION_MACHINE_RESUME || kind == SPG_ACTION_MACHINE_FAN;
}

static uint64_t consumed_for_action(const struct spg_policy_usage *usage,
                                    const enum spg_action_kind kind) {
    switch (kind) {
    case SPG_ACTION_LOCAL_SHELL:
        return usage->consumed.shell_actions;
    case SPG_ACTION_SSH_AUTH_PROBE:
        return usage->consumed.shell_actions;
    case SPG_ACTION_SIMULATOR:
        return usage->consumed.sim_actions;
    case SPG_ACTION_MEMORY_SAVE:
    case SPG_ACTION_MEMORY_DELETE:
    case SPG_ACTION_MEMORY_READ:
        return usage->consumed.memory_actions;
    case SPG_ACTION_MACHINE_PAUSE:
    case SPG_ACTION_MACHINE_RESUME:
    case SPG_ACTION_MACHINE_FAN:
        return usage->consumed.machine_actions;
    default:
        return UINT64_MAX;
    }
}

static uint64_t global_budget_for_action(const struct spg_policy_config *policy,
                                         const enum spg_action_kind kind) {
    switch (kind) {
    case SPG_ACTION_LOCAL_SHELL:
        return policy->budgets.shell_actions;
    case SPG_ACTION_SSH_AUTH_PROBE:
        return policy->budgets.shell_actions;
    case SPG_ACTION_SIMULATOR:
        return policy->budgets.sim_actions;
    case SPG_ACTION_MEMORY_SAVE:
    case SPG_ACTION_MEMORY_DELETE:
    case SPG_ACTION_MEMORY_READ:
        return policy->budgets.memory_actions;
    case SPG_ACTION_MACHINE_PAUSE:
    case SPG_ACTION_MACHINE_RESUME:
    case SPG_ACTION_MACHINE_FAN:
        return policy->budgets.machine_actions;
    default:
        return 0u;
    }
}

static bool would_exceed(const uint64_t used, const uint64_t cost,
                         const uint64_t budget) {
    if (cost > UINT64_MAX - used) {
        return true;
    }
    return used + cost > budget;
}

const char *spg_action_kind_to_string(const enum spg_action_kind kind) {
    switch (kind) {
    case SPG_ACTION_LOCAL_SHELL:
        return "local_shell";
    case SPG_ACTION_SSH_AUTH_PROBE:
        return "ssh_auth_probe";
    case SPG_ACTION_SIMULATOR:
        return "simulator";
    case SPG_ACTION_MEMORY_SAVE:
        return "memory_save";
    case SPG_ACTION_MEMORY_DELETE:
        return "memory_delete";
    case SPG_ACTION_MEMORY_READ:
        return "memory_read";
    case SPG_ACTION_MACHINE_PAUSE:
        return "machine_pause_process";
    case SPG_ACTION_MACHINE_RESUME:
        return "machine_resume_process";
    case SPG_ACTION_FINISH:
        return "finish";
    }
    return "unknown";
}

const char *spg_policy_deny_reason_to_string(
    const enum spg_policy_deny_reason reason) {
    switch (reason) {
    case SPG_POLICY_DENY_NONE:
        return "SPG_POLICY_DENY_NONE";
    case SPG_POLICY_DENY_UNKNOWN_CAPABILITY:
        return "SPG_POLICY_DENY_UNKNOWN_CAPABILITY";
    case SPG_POLICY_DENY_DISABLED_CAPABILITY:
        return "SPG_POLICY_DENY_DISABLED_CAPABILITY";
    case SPG_POLICY_DENY_KIND_MISMATCH:
        return "SPG_POLICY_DENY_KIND_MISMATCH";
    case SPG_POLICY_DENY_NETWORK:
        return "SPG_POLICY_DENY_NETWORK";
    case SPG_POLICY_DENY_CAPABILITY_BUDGET:
        return "SPG_POLICY_DENY_CAPABILITY_BUDGET";
    case SPG_POLICY_DENY_GLOBAL_BUDGET:
        return "SPG_POLICY_DENY_GLOBAL_BUDGET";
    case SPG_POLICY_DENY_INVALID_REQUEST:
        return "SPG_POLICY_DENY_INVALID_REQUEST";
    case SPG_POLICY_DENY_UNMANAGED_PROCESS:
        return "SPG_POLICY_DENY_UNMANAGED_PROCESS";
    case SPG_POLICY_DENY_PROCESS_PROTECTED:
        return "SPG_POLICY_DENY_PROCESS_PROTECTED";
    case SPG_POLICY_DENY_PROCESS_IDENTITY:
        return "SPG_POLICY_DENY_PROCESS_IDENTITY";
    default:
        return "SPG_POLICY_DENY_UNKNOWN";
    }
}

enum spg_status spg_policy_decide(
    const size_t policy_text_n, const char policy_text[],
    const struct spg_policy_config *policy,
    const struct spg_policy_usage *usage,
    const struct spg_action_request *request,
    struct spg_policy_decision *decision) {
    if (policy_text == nullptr || policy == nullptr || usage == nullptr ||
        request == nullptr || decision == nullptr) {
        return SPG_E_INVALID_ARG;
    }

    deny(decision, SPG_POLICY_DENY_INVALID_REQUEST);

    if (!action_kind_valid(request->kind) || request->cost == 0u ||
        !spg_sexpr_span_valid(policy_text_n, request->capability)) {
        return SPG_OK;
    }

    if (request->uses_network &&
        policy->network_default == SPG_POLICY_NETWORK_DENY &&
        request->kind != SPG_ACTION_SSH_AUTH_PROBE) {
        deny(decision, SPG_POLICY_DENY_NETWORK);
        return SPG_OK;
    }

    size_t cap_index = SIZE_MAX;
    for (size_t i = 0u; i < policy->capability_count; i += 1u) {
        if (span_eq(policy_text_n, policy_text, policy->capabilities[i].name,
                    request->capability)) {
            cap_index = i;
            break;
        }
    }
    if (cap_index == SIZE_MAX) {
        deny(decision, SPG_POLICY_DENY_UNKNOWN_CAPABILITY);
        return SPG_OK;
    }

    const struct spg_policy_capability *cap = &policy->capabilities[cap_index];
    if (!cap->enabled) {
        deny(decision, SPG_POLICY_DENY_DISABLED_CAPABILITY);
        decision->capability_index = cap_index;
        return SPG_OK;
    }
    if (cap->kind != cap_kind_for_action(request->kind)) {
        deny(decision, SPG_POLICY_DENY_KIND_MISMATCH);
        decision->capability_index = cap_index;
        return SPG_OK;
    }
    if (would_exceed(0u, request->cost, cap->budget)) {
        deny(decision, SPG_POLICY_DENY_CAPABILITY_BUDGET);
        decision->capability_index = cap_index;
        return SPG_OK;
    }

    const uint64_t used          = consumed_for_action(usage, request->kind);
    const uint64_t global_budget = global_budget_for_action(policy, request->kind);
    if (would_exceed(used, request->cost, global_budget)) {
        deny(decision, SPG_POLICY_DENY_GLOBAL_BUDGET);
        decision->capability_index = cap_index;
        return SPG_OK;
    }

    allow(decision, cap_index);
    return SPG_OK;
}
