#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#    define _DARWIN_C_SOURCE 1
#endif

#include "geistshell/improve.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* A lesson is produced for each failing termination mode, with a valid slug. */
static int test_reflect_failure_modes(void) {
    const struct {
        enum spg_agent_loop_termination term;
        const char                     *slug;
    } cases[] = {
        {SPG_AGENT_LOOP_REJECTED, "lesson-rejected"},
        {SPG_AGENT_LOOP_DENIED, "lesson-denied"},
        {SPG_AGENT_LOOP_BUDGET, "lesson-budget"},
        {SPG_AGENT_LOOP_MAX_STEPS, "lesson-max-steps"},
        {SPG_AGENT_LOOP_ERROR, "lesson-error"},
    };
    for (size_t i = 0u; i < sizeof cases / sizeof cases[0]; i += 1u) {
        const struct spg_eval_case_result result = {
            .outcome     = SPG_EVAL_FAIL_TERMINATION,
            .termination = cases[i].term,
        };
        struct spg_lesson lesson = {};
        if (!spg_reflect_case(&result, &lesson)) {
            return 1;
        }
        if (strcmp(lesson.slug, cases[i].slug) != 0) {
            return 1;
        }
        /* slug is mind-palace-safe, description has no newline, body non-empty */
        if (!spg_mem_slug_valid(lesson.slug) ||
            strchr(lesson.description, '\n') != nullptr ||
            lesson.body[0] == '\0') {
            return 1;
        }
    }
    return 0;
}

/* Lessons are earned: the concrete failure signal (reject/deny reason, step and
 * repair counts) is woven into the text, not a constant template. */
static int test_reflect_earned_content(void) {
    struct spg_lesson lesson = {};

    const struct spg_eval_case_result rejected = {
        .outcome       = SPG_EVAL_FAIL_TERMINATION,
        .termination   = SPG_AGENT_LOOP_REJECTED,
        .reject_reason = SPG_RECOMMENDATION_REJECT_MISSING_FIELD,
        .repairs_used  = 2u,
    };
    if (!spg_reflect_case(&rejected, &lesson) ||
        strcmp(lesson.slug, "lesson-rejected") != 0 ||
        strstr(lesson.description, "a required field was missing") == nullptr ||
        strstr(lesson.body, "a required field was missing") == nullptr) {
        return 1;
    }

    const struct spg_eval_case_result denied = {
        .outcome     = SPG_EVAL_FAIL_TERMINATION,
        .termination = SPG_AGENT_LOOP_DENIED,
        .deny_reason = SPG_POLICY_DENY_DISABLED_CAPABILITY,
    };
    if (!spg_reflect_case(&denied, &lesson) ||
        strcmp(lesson.slug, "lesson-denied") != 0 ||
        strstr(lesson.body, "the capability is disabled") == nullptr) {
        return 1;
    }

    const struct spg_eval_case_result capped = {
        .outcome     = SPG_EVAL_FAIL_TERMINATION,
        .termination = SPG_AGENT_LOOP_MAX_STEPS,
        .steps_taken = 7u,
    };
    if (!spg_reflect_case(&capped, &lesson) ||
        strstr(lesson.body, "7 step") == nullptr) {
        return 1;
    }

    /* Distinct reasons in the same failure mode produce distinct lesson text
     * (the whole point of "earned": same slug, different diagnosis). */
    struct spg_lesson                 from_missing = {};
    struct spg_lesson                 from_syntax  = {};
    const struct spg_eval_case_result rejected_syntax = {
        .outcome       = SPG_EVAL_FAIL_TERMINATION,
        .termination   = SPG_AGENT_LOOP_REJECTED,
        .reject_reason = SPG_RECOMMENDATION_REJECT_SYNTAX,
    };
    if (!spg_reflect_case(&rejected, &from_missing) ||
        !spg_reflect_case(&rejected_syntax, &from_syntax) ||
        strcmp(from_missing.slug, from_syntax.slug) != 0 ||
        strcmp(from_missing.body, from_syntax.body) == 0) {
        return 1; /* same slug, but MISSING_FIELD vs SYNTAX must read differently */
    }
    return 0;
}

/* A passing case yields no lesson. */
static int test_reflect_pass_no_lesson(void) {
    const struct spg_eval_case_result result = {
        .outcome     = SPG_EVAL_PASS,
        .termination = SPG_AGENT_LOOP_FINISHED,
    };
    struct spg_lesson lesson = {};
    return spg_reflect_case(&result, &lesson) ? 1 : 0;
}

/* Finished-but-expectation-mismatch yields no agent lesson. */
static int test_reflect_finished_no_lesson(void) {
    const struct spg_eval_case_result result = {
        .outcome     = SPG_EVAL_FAIL_OBSERVATION,
        .termination = SPG_AGENT_LOOP_FINISHED,
    };
    struct spg_lesson lesson = {};
    return spg_reflect_case(&result, &lesson) ? 1 : 0;
}

static int test_accept_gate(void) {
    /* keep on improvement and on no-change; revert on regression */
    if (!spg_improve_accept(2u, 3u) || !spg_improve_accept(2u, 2u) ||
        spg_improve_accept(2u, 1u)) {
        return 1;
    }
    return 0;
}

static int test_reflect_null_args(void) {
    struct spg_lesson lesson = {};
    const struct spg_eval_case_result result = {.outcome =
                                                    SPG_EVAL_FAIL_TERMINATION};
    return (spg_reflect_case(nullptr, &lesson) ||
            spg_reflect_case(&result, nullptr))
               ? 1
               : 0;
}

/* commit keeps an accepted lesson and deletes (reverts) a rejected one. */
static int test_commit_keep_and_revert(void) {
    struct spg_mem_store store;
    char                 dir[64];
    memcpy(dir, "/tmp/spg_improve_XXXXXX", 23u);
    if (mkdtemp(dir) == nullptr ||
        spg_mem_store_open(&store, dir) != SPG_OK) {
        return 1;
    }
    const struct spg_lesson lesson = {
        .slug = "lesson-rejected", .description = "d", .body = "b"};

    /* accepted -> the (already saved) lesson stays */
    if (spg_mem_save(&store, lesson.slug, lesson.description, lesson.body) !=
        SPG_OK) {
        return 1;
    }
    bool kept = false;
    char body[64];
    if (spg_improve_commit(&store, &lesson, true, &kept) != SPG_OK || !kept ||
        spg_mem_read(&store, lesson.slug, sizeof body, body, nullptr) !=
            SPG_OK) {
        return 1;
    }

    /* rejected -> the lesson is reverted (deleted) */
    if (spg_improve_commit(&store, &lesson, false, &kept) != SPG_OK || kept ||
        spg_mem_read(&store, lesson.slug, sizeof body, body, nullptr) !=
            SPG_E_NOT_FOUND) {
        return 1;
    }
    /* reverting an absent lesson is not an error */
    if (spg_improve_commit(&store, &lesson, false, &kept) != SPG_OK) {
        return 1;
    }

    char cmd[128];
    (void)snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    (void)system(cmd);
    return 0;
}

int main(void) {
    if (test_reflect_failure_modes() != 0) {
        fprintf(stderr, "test_reflect_failure_modes failed\n");
        return 1;
    }
    if (test_reflect_earned_content() != 0) {
        fprintf(stderr, "test_reflect_earned_content failed\n");
        return 1;
    }
    if (test_reflect_pass_no_lesson() != 0) {
        fprintf(stderr, "test_reflect_pass_no_lesson failed\n");
        return 1;
    }
    if (test_reflect_finished_no_lesson() != 0) {
        fprintf(stderr, "test_reflect_finished_no_lesson failed\n");
        return 1;
    }
    if (test_accept_gate() != 0) {
        fprintf(stderr, "test_accept_gate failed\n");
        return 1;
    }
    if (test_reflect_null_args() != 0) {
        fprintf(stderr, "test_reflect_null_args failed\n");
        return 1;
    }
    if (test_commit_keep_and_revert() != 0) {
        fprintf(stderr, "test_commit_keep_and_revert failed\n");
        return 1;
    }
    return 0;
}
