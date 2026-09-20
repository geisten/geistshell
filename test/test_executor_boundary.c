/* mkdir(2), symlink(2) and friends are POSIX; -std=c23 alone hides them. */
#define _POSIX_C_SOURCE 200809L

#include "geistshell/executor_boundary.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The workdir check canonicalises both sides, so these cases need directories
 * that actually exist. The layout is built once and mirrors the two bypasses
 * the plain prefix compare allowed:
 *
 *   <root>/allowed          the configured boundary
 *   <root>/allowed/lab      legitimately inside it
 *   <root>/allowed-evil     a sibling whose name starts with the boundary
 *   <root>/outside          the target of an escape
 *   <root>/allowed/escape   a symlink out of the boundary
 */
static char root[256];
static char allowed[320];
static char inside[384];
static char sibling[384];
static char outside[384];
static char symlink_escape[448];
static char dotdot[448];
static char missing[448];

static int make_layout(void) {
    (void)snprintf(root, sizeof root, "/tmp/geistshell-boundary-%ld",
                   (long)getpid());
    (void)snprintf(allowed, sizeof allowed, "%s/allowed", root);
    (void)snprintf(inside, sizeof inside, "%s/lab", allowed);
    (void)snprintf(sibling, sizeof sibling, "%s/allowed-evil", root);
    (void)snprintf(outside, sizeof outside, "%s/outside", root);
    (void)snprintf(symlink_escape, sizeof symlink_escape, "%s/escape", allowed);
    (void)snprintf(dotdot, sizeof dotdot, "%s/../outside", allowed);
    (void)snprintf(missing, sizeof missing, "%s/ghost", allowed);

    if (mkdir(root, 0700) != 0 || mkdir(allowed, 0700) != 0 ||
        mkdir(inside, 0700) != 0 || mkdir(sibling, 0700) != 0 ||
        mkdir(outside, 0700) != 0) {
        return 1;
    }
    return symlink(outside, symlink_escape) == 0 ? 0 : 1;
}

static void remove_layout(void) {
    (void)unlink(symlink_escape);
    (void)rmdir(inside);
    (void)rmdir(allowed);
    (void)rmdir(sibling);
    (void)rmdir(outside);
    (void)rmdir(root);
}

static struct spg_recommendation local_shell_recommendation(void) {
    return (struct spg_recommendation){
        .state       = SPG_RECOMMENDATION_VALID,
        .action_kind = SPG_ACTION_LOCAL_SHELL,
        .action      = {.kind         = SPG_ACTION_LOCAL_SHELL,
                        .uses_network = false,
                        .cost         = 1u},
        .command     = {.offset = 0u, .length = strlen("make test")},
        .has_command = true,
    };
}

static struct spg_policy_decision allow_policy(void) {
    return (struct spg_policy_decision){
        .kind        = SPG_POLICY_DECISION_ALLOW,
        .deny_reason = SPG_POLICY_DENY_NONE,
    };
}

static struct spg_executor_boundary_config boundary_config(void) {
    return (struct spg_executor_boundary_config){
        .execution_enabled      = true,
        .allowed_workdir_prefix = allowed,
        .max_timeout_ms         = 1000u,
        .max_stdout_bytes       = 4096u,
        .max_stderr_bytes       = 4096u,
        .require_clean_env      = true,
    };
}

static struct spg_executor_boundary_request boundary_request(void) {
    return (struct spg_executor_boundary_request){
        .working_dir        = inside,
        .timeout_ms         = 250u,
        .stdout_limit_bytes = 1024u,
        .stderr_limit_bytes = 1024u,
        .env_cleared        = true,
    };
}

/* Run the gate with `working_dir` and report whether it denied with
 * bad_workdir. Every workdir case is the same three lines otherwise. */
static int denies_workdir(const char *working_dir) {
    struct spg_recommendation rec = local_shell_recommendation();
    struct spg_policy_decision policy = allow_policy();
    struct spg_executor_boundary_config config = boundary_config();
    struct spg_executor_boundary_request request = boundary_request();
    struct spg_executor_boundary_plan plan = {};
    request.working_dir = working_dir;

    if (spg_executor_boundary_check(&config, &rec, &policy, &request, &plan) !=
        SPG_OK) {
        return 1;
    }
    return !plan.approved && plan.reason == SPG_EXECUTOR_BOUNDARY_BAD_WORKDIR
               ? 0
               : 1;
}

static int approves_workdir(const char *working_dir) {
    struct spg_recommendation rec = local_shell_recommendation();
    struct spg_policy_decision policy = allow_policy();
    struct spg_executor_boundary_config config = boundary_config();
    struct spg_executor_boundary_request request = boundary_request();
    struct spg_executor_boundary_plan plan = {};
    request.working_dir = working_dir;

    if (spg_executor_boundary_check(&config, &rec, &policy, &request, &plan) !=
        SPG_OK) {
        return 1;
    }
    return plan.approved && plan.reason == SPG_EXECUTOR_BOUNDARY_OK ? 0 : 1;
}

static int test_default_denies_execution(void) {
    struct spg_recommendation rec = local_shell_recommendation();
    struct spg_policy_decision policy = allow_policy();
    struct spg_executor_boundary_config config = boundary_config();
    struct spg_executor_boundary_request request = boundary_request();
    struct spg_executor_boundary_plan plan = {};
    config.execution_enabled = false;

    if (spg_executor_boundary_check(&config, &rec, &policy, &request, &plan) !=
        SPG_OK) {
        return 1;
    }
    return !plan.approved &&
                   plan.reason == SPG_EXECUTOR_BOUNDARY_EXECUTION_DISABLED
               ? 0
               : 1;
}

static int test_allows_when_explicitly_enabled_and_bounded(void) {
    return approves_workdir(inside);
}

/* The boundary directory itself is inside the boundary. */
static int test_allows_the_boundary_itself(void) {
    return approves_workdir(allowed);
}

static int test_rejects_bad_workdir(void) {
    return denies_workdir("/etc");
}

/* A sibling that merely starts with the boundary string. The plain strncmp
 * accepted this: "<root>/allowed-evil" begins with "<root>/allowed". */
static int test_rejects_sibling_sharing_the_prefix(void) {
    return denies_workdir(sibling);
}

/* "<boundary>/../outside" never touched the filesystem before, so the prefix
 * matched while the path pointed out of the sandbox. */
static int test_rejects_dotdot_escape(void) {
    return denies_workdir(dotdot);
}

/* A symlink inside the boundary pointing out of it. Only canonicalisation
 * catches this; no lexical check can. */
static int test_rejects_symlink_escape(void) {
    return denies_workdir(symlink_escape);
}

/* Fail-closed: a path that does not resolve is refused. The gate runs right
 * before execution, so such a directory could not be entered anyway. */
static int test_rejects_unresolvable_workdir(void) {
    return denies_workdir(missing);
}

static int test_invalid_args(void) {
    struct spg_recommendation rec = local_shell_recommendation();
    struct spg_policy_decision policy = allow_policy();
    struct spg_executor_boundary_request request = boundary_request();
    struct spg_executor_boundary_plan plan = {};
    if (spg_executor_boundary_check(nullptr, &rec, &policy, &request, &plan) !=
        SPG_E_INVALID_ARG) {
        return 1;
    }
    return spg_executor_boundary_reason_to_string(
               SPG_EXECUTOR_BOUNDARY_BAD_TIMEOUT) != nullptr
               ? 0
               : 1;
}

struct case_entry {
    const char *name;
    int (*fn)(void);
};

int main(void) {
    if (make_layout() != 0) {
        fprintf(stderr, "could not build the test layout under %s\n", root);
        remove_layout();
        return 1;
    }

    const struct case_entry cases[] = {
        {"test_default_denies_execution", test_default_denies_execution},
        {"test_allows_when_explicitly_enabled_and_bounded",
         test_allows_when_explicitly_enabled_and_bounded},
        {"test_allows_the_boundary_itself", test_allows_the_boundary_itself},
        {"test_rejects_bad_workdir", test_rejects_bad_workdir},
        {"test_rejects_sibling_sharing_the_prefix",
         test_rejects_sibling_sharing_the_prefix},
        {"test_rejects_dotdot_escape", test_rejects_dotdot_escape},
        {"test_rejects_symlink_escape", test_rejects_symlink_escape},
        {"test_rejects_unresolvable_workdir", test_rejects_unresolvable_workdir},
        {"test_invalid_args", test_invalid_args},
    };

    int rc = 0;
    for (size_t i = 0u; i < sizeof cases / sizeof cases[0]; i += 1u) {
        if (cases[i].fn() != 0) {
            fprintf(stderr, "%s failed\n", cases[i].name);
            rc = 1;
            break;
        }
    }
    remove_layout();
    return rc;
}
