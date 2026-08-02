#include "geistshell/improve.h"

#include "geistshell/policy.h"
#include "geistshell/recommendation.h"

#include <stdio.h>
#include <string.h>

/* Concise, model-readable phrase for why a recommendation was rejected — the
 * concrete signal distilled into the lesson, instead of leaking the internal
 * enum name. */
static const char *reject_phrase(const enum spg_recommendation_reject_reason r) {
    switch (r) {
    case SPG_RECOMMENDATION_REJECT_EMPTY:
        return "the reply was empty";
    case SPG_RECOMMENDATION_REJECT_SYNTAX:
        return "the s-expression did not parse";
    case SPG_RECOMMENDATION_REJECT_SCHEMA:
        return "the form did not match the (recommend ...) schema";
    case SPG_RECOMMENDATION_REJECT_UNKNOWN_KIND:
        return "the action kind was not recognised";
    case SPG_RECOMMENDATION_REJECT_MISSING_FIELD:
        return "a required field was missing";
    case SPG_RECOMMENDATION_REJECT_DUPLICATE_FIELD:
        return "a field was given twice";
    case SPG_RECOMMENDATION_REJECT_WRONG_VALUE:
        return "a field had an invalid value";
    case SPG_RECOMMENDATION_REJECT_KIND_MISMATCH:
        return "the fields did not match the action kind";
    case SPG_RECOMMENDATION_REJECT_NONE:
    default:
        return "no specific reason was recorded";
    }
}

/* Concise, model-readable phrase for why an action was denied by policy. */
static const char *deny_phrase(const enum spg_policy_deny_reason r) {
    switch (r) {
    case SPG_POLICY_DENY_UNKNOWN_CAPABILITY:
        return "the capability is not in the policy";
    case SPG_POLICY_DENY_DISABLED_CAPABILITY:
        return "the capability is disabled";
    case SPG_POLICY_DENY_KIND_MISMATCH:
        return "the capability does not match the action kind";
    case SPG_POLICY_DENY_NETWORK:
        return "network use is not permitted";
    case SPG_POLICY_DENY_CAPABILITY_BUDGET:
        return "the capability budget is exhausted";
    case SPG_POLICY_DENY_GLOBAL_BUDGET:
        return "the global budget is exhausted";
    case SPG_POLICY_DENY_INVALID_REQUEST:
        return "the request was malformed";
    case SPG_POLICY_DENY_NONE:
    default:
        return "no specific reason was recorded";
    }
}

enum spg_status spg_improve_commit(struct spg_mem_store *store,
                                   const struct spg_lesson *lesson,
                                   const bool accepted, bool *kept) {
    if (store == nullptr || lesson == nullptr || kept == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    if (accepted) {
        *kept = true;
        return SPG_OK;
    }
    *kept                 = false;
    const enum spg_status s = spg_mem_delete(store, lesson->slug);
    /* A revert of a never-saved lesson is fine. */
    return (s == SPG_OK || s == SPG_E_NOT_FOUND) ? SPG_OK : SPG_E_IO;
}

bool spg_reflect_case(const struct spg_eval_case_result *result,
                      struct spg_lesson *out) {
    if (result == nullptr || out == nullptr ||
        result->outcome == SPG_EVAL_PASS) {
        return false;
    }
    /* Each lesson keeps a fixed slug per failure mode (so dedup, the keep/revert
     * gate, and recall stay stable) but its description and body are *earned*:
     * composed from the concrete signal of the run that failed — the reject/deny
     * reason and the step/repair counts — not a constant template. */
    switch (result->termination) {
    case SPG_AGENT_LOOP_REJECTED: {
        const char *why = reject_phrase(result->reject_reason);
        (void)snprintf(out->slug, sizeof out->slug, "%s", "lesson-rejected");
        (void)snprintf(out->description, sizeof out->description,
                       "Emit exactly one valid recommendation s-expression "
                       "(last reply rejected: %s).",
                       why);
        (void)snprintf(
            out->body, sizeof out->body,
            "A previous reply was rejected as malformed: %s (after %zu "
            "self-repair attempt(s)). Reply with exactly one valid (recommend "
            "(kind <action>) (capability \"...\") (cost 1) (uses_network false) "
            "(confidence_bp <n>) (reason \"...\")) form, or (recommend (kind "
            "finish) (reason \"...\")) when the task is done. Output only the "
            "s-expression and nothing else.",
            why, result->repairs_used);
        return true;
    }
    case SPG_AGENT_LOOP_DENIED: {
        const char *why = deny_phrase(result->deny_reason);
        (void)snprintf(out->slug, sizeof out->slug, "%s", "lesson-denied");
        (void)snprintf(out->description, sizeof out->description,
                       "Recommend only actions whose capability the policy "
                       "allows (last denial: %s).",
                       why);
        (void)snprintf(
            out->body, sizeof out->body,
            "A previous action was denied by policy: %s. Recommend only actions "
            "whose capability is enabled and matches the action kind; when "
            "unsure, prefer a simulator or a finish action over one that gets "
            "denied.",
            why);
        return true;
    }
    case SPG_AGENT_LOOP_BUDGET:
        (void)snprintf(out->slug, sizeof out->slug, "%s", "lesson-budget");
        (void)snprintf(out->description, sizeof out->description,
                       "Finish before the step or token budget runs out (spent "
                       "%zu step(s)).",
                       result->steps_taken);
        (void)snprintf(
            out->body, sizeof out->body,
            "A previous run exhausted its budget after %zu step(s) without "
            "finishing. Take the most direct path to the goal and emit "
            "(recommend (kind finish) ...) as soon as the task is complete, "
            "instead of spending steps on redundant actions.",
            result->steps_taken);
        return true;
    case SPG_AGENT_LOOP_MAX_STEPS:
        (void)snprintf(out->slug, sizeof out->slug, "%s", "lesson-max-steps");
        (void)snprintf(out->description, sizeof out->description,
                       "Reach the finish action in fewer steps (hit the cap "
                       "after %zu step(s)).",
                       result->steps_taken);
        (void)snprintf(
            out->body, sizeof out->body,
            "A previous run hit the step cap after %zu step(s) without "
            "finishing. Plan the shortest sequence of actions and finish "
            "promptly; do not repeat an action that already succeeded.",
            result->steps_taken);
        return true;
    case SPG_AGENT_LOOP_ERROR:
        (void)snprintf(out->slug, sizeof out->slug, "%s", "lesson-error");
        (void)snprintf(out->description, sizeof out->description,
                       "Re-check an action's required fields to avoid run "
                       "errors.");
        (void)snprintf(
            out->body, sizeof out->body,
            "A previous run ended with an internal error after %zu step(s). "
            "Verify each action's required fields and prefer a simpler, "
            "well-formed action.",
            result->steps_taken);
        return true;
    case SPG_AGENT_LOOP_FINISHED:
        /* Finished cleanly but missed an eval expectation: no agent-level
         * lesson to learn from the failure mode. */
        return false;
    }
    return false;
}
