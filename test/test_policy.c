#include "geistshell/policy.h"
#include "geistshell/policy_config.h"

#include <stdio.h>
#include <string.h>

static const char policy_text[] =
    "(policy"
    " (network_default deny)"
    " (budgets"
    "  (inference_steps 100)"
    "  (tokens 4096)"
    "  (shell_actions 8)"
    "  (sim_actions 250)"
    "  (wall_ms 60000)"
    "  (journal_bytes 1048576)"
    "  (risk_bp 10000))"
    " (capability"
    "  ((name build.run) (kind local_shell) (enabled true) (budget 8))"
    "  ((name auth_probe.ssh_publickey_single) (kind ssh_auth_probe)"
    "   (enabled false) (budget 1))"
    "  ((name sim.act) (kind simulator) (enabled true) (budget 250))))";

static int load_policy(struct spg_policy_config *config) {
    struct spg_sexpr_token        tokens[192];
    struct spg_sexpr_node         nodes[192];
    struct spg_policy_config_error error = {};
    return spg_policy_config_load(strlen(policy_text), policy_text, 192u,
                                  tokens, 192u, nodes, config, &error) == SPG_OK
               ? 0
               : 1;
}

static struct spg_text_span span_for(const char *needle) {
    const char *found = strstr(policy_text, needle);
    if (found == nullptr) {
        return (struct spg_text_span){.offset = 0u, .length = 0u};
    }
    return (struct spg_text_span){
        .offset = (size_t)(found - policy_text),
        .length = strlen(needle),
    };
}

static int decide(const struct spg_policy_config *config,
                  const struct spg_policy_usage *usage,
                  const struct spg_action_request *request,
                  struct spg_policy_decision *decision) {
    return spg_policy_decide(strlen(policy_text), policy_text, config, usage,
                             request, decision) == SPG_OK
               ? 0
               : 1;
}

static int test_allow_shell(void) {
    struct spg_policy_config   config = {};
    struct spg_policy_usage    usage  = {};
    struct spg_policy_decision decision = {};
    if (load_policy(&config) != 0) {
        return 1;
    }
    const struct spg_action_request request = {
        .kind         = SPG_ACTION_LOCAL_SHELL,
        .capability   = span_for("build.run"),
        .uses_network = false,
        .cost         = 1u,
    };
    if (decide(&config, &usage, &request, &decision) != 0) {
        return 1;
    }
    return decision.kind == SPG_POLICY_DECISION_ALLOW &&
                   decision.deny_reason == SPG_POLICY_DENY_NONE
               ? 0
               : 1;
}

static int test_unknown_capability(void) {
    struct spg_policy_config   config = {};
    struct spg_policy_usage    usage  = {};
    struct spg_policy_decision decision = {};
    if (load_policy(&config) != 0) {
        return 1;
    }
    const struct spg_action_request request = {
        .kind         = SPG_ACTION_LOCAL_SHELL,
        .capability   = span_for("missing"),
        .uses_network = false,
        .cost         = 1u,
    };
    if (decide(&config, &usage, &request, &decision) != 0) {
        return 1;
    }
    return decision.kind == SPG_POLICY_DECISION_DENY &&
                   decision.deny_reason == SPG_POLICY_DENY_UNKNOWN_CAPABILITY
               ? 0
               : 1;
}

static int test_disabled_capability(void) {
    struct spg_policy_config   config = {};
    struct spg_policy_usage    usage  = {};
    struct spg_policy_decision decision = {};
    if (load_policy(&config) != 0) {
        return 1;
    }
    const struct spg_action_request request = {
        .kind         = SPG_ACTION_SSH_AUTH_PROBE,
        .capability   = span_for("auth_probe.ssh_publickey_single"),
        .uses_network = true,
        .cost         = 1u,
    };
    if (decide(&config, &usage, &request, &decision) != 0) {
        return 1;
    }
    return decision.kind == SPG_POLICY_DECISION_DENY &&
                   decision.deny_reason == SPG_POLICY_DENY_DISABLED_CAPABILITY
               ? 0
               : 1;
}

static int test_kind_mismatch(void) {
    struct spg_policy_config   config = {};
    struct spg_policy_usage    usage  = {};
    struct spg_policy_decision decision = {};
    if (load_policy(&config) != 0) {
        return 1;
    }
    const struct spg_action_request request = {
        .kind         = SPG_ACTION_SIMULATOR,
        .capability   = span_for("build.run"),
        .uses_network = false,
        .cost         = 1u,
    };
    if (decide(&config, &usage, &request, &decision) != 0) {
        return 1;
    }
    return decision.kind == SPG_POLICY_DECISION_DENY &&
                   decision.deny_reason == SPG_POLICY_DENY_KIND_MISMATCH
               ? 0
               : 1;
}

static int test_network_denied_for_shell(void) {
    struct spg_policy_config   config = {};
    struct spg_policy_usage    usage  = {};
    struct spg_policy_decision decision = {};
    if (load_policy(&config) != 0) {
        return 1;
    }
    const struct spg_action_request request = {
        .kind         = SPG_ACTION_LOCAL_SHELL,
        .capability   = span_for("build.run"),
        .uses_network = true,
        .cost         = 1u,
    };
    if (decide(&config, &usage, &request, &decision) != 0) {
        return 1;
    }
    return decision.kind == SPG_POLICY_DECISION_DENY &&
                   decision.deny_reason == SPG_POLICY_DENY_NETWORK
               ? 0
               : 1;
}

static int test_capability_budget_exceeded(void) {
    struct spg_policy_config   config = {};
    struct spg_policy_usage    usage  = {};
    struct spg_policy_decision decision = {};
    if (load_policy(&config) != 0) {
        return 1;
    }
    const struct spg_action_request request = {
        .kind         = SPG_ACTION_LOCAL_SHELL,
        .capability   = span_for("build.run"),
        .uses_network = false,
        .cost         = 9u,
    };
    if (decide(&config, &usage, &request, &decision) != 0) {
        return 1;
    }
    return decision.kind == SPG_POLICY_DECISION_DENY &&
                   decision.deny_reason == SPG_POLICY_DENY_CAPABILITY_BUDGET
               ? 0
               : 1;
}

static int test_global_budget_exceeded(void) {
    struct spg_policy_config   config = {};
    struct spg_policy_usage    usage  = {};
    struct spg_policy_decision decision = {};
    if (load_policy(&config) != 0) {
        return 1;
    }
    usage.consumed.shell_actions = 8u;
    const struct spg_action_request request = {
        .kind         = SPG_ACTION_LOCAL_SHELL,
        .capability   = span_for("build.run"),
        .uses_network = false,
        .cost         = 1u,
    };
    if (decide(&config, &usage, &request, &decision) != 0) {
        return 1;
    }
    return decision.kind == SPG_POLICY_DECISION_DENY &&
                   decision.deny_reason == SPG_POLICY_DENY_GLOBAL_BUDGET
               ? 0
               : 1;
}

static int test_invalid_request(void) {
    struct spg_policy_config   config = {};
    struct spg_policy_usage    usage  = {};
    struct spg_policy_decision decision = {};
    if (load_policy(&config) != 0) {
        return 1;
    }
    const struct spg_action_request request = {
        .kind         = SPG_ACTION_LOCAL_SHELL,
        .capability   = span_for("build.run"),
        .uses_network = false,
        .cost         = 0u,
    };
    if (decide(&config, &usage, &request, &decision) != 0) {
        return 1;
    }
    return decision.kind == SPG_POLICY_DECISION_DENY &&
                   decision.deny_reason == SPG_POLICY_DENY_INVALID_REQUEST
               ? 0
               : 1;
}

static int test_invalid_args(void) {
    struct spg_policy_config   config = {};
    struct spg_policy_usage    usage  = {};
    struct spg_policy_decision decision = {};
    struct spg_action_request  request = {};
    return spg_policy_decide(0u, nullptr, &config, &usage, &request,
                             &decision) == SPG_E_INVALID_ARG
               ? 0
               : 1;
}

int main(void) {
    if (test_allow_shell() != 0) {
        fprintf(stderr, "test_allow_shell failed\n");
        return 1;
    }
    if (test_unknown_capability() != 0) {
        fprintf(stderr, "test_unknown_capability failed\n");
        return 1;
    }
    if (test_disabled_capability() != 0) {
        fprintf(stderr, "test_disabled_capability failed\n");
        return 1;
    }
    if (test_kind_mismatch() != 0) {
        fprintf(stderr, "test_kind_mismatch failed\n");
        return 1;
    }
    if (test_network_denied_for_shell() != 0) {
        fprintf(stderr, "test_network_denied_for_shell failed\n");
        return 1;
    }
    if (test_capability_budget_exceeded() != 0) {
        fprintf(stderr, "test_capability_budget_exceeded failed\n");
        return 1;
    }
    if (test_global_budget_exceeded() != 0) {
        fprintf(stderr, "test_global_budget_exceeded failed\n");
        return 1;
    }
    if (test_invalid_request() != 0) {
        fprintf(stderr, "test_invalid_request failed\n");
        return 1;
    }
    if (test_invalid_args() != 0) {
        fprintf(stderr, "test_invalid_args failed\n");
        return 1;
    }
    if (spg_policy_deny_reason_to_string(SPG_POLICY_DENY_NETWORK) ==
        nullptr) {
        fprintf(stderr, "deny reason string failed\n");
        return 1;
    }
    return 0;
}
