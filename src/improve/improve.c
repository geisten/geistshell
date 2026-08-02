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
        /* Finished cleanly but missed an eval expectation: the failure is in
         * the outcome, not the termination — spg_reflect_outcome handles it. */
        return false;
    }
    return false;
}

/* Append a slug-safe rendering of token to dst: [a-z0-9] kept (lowercased),
 * every other run collapses to a single '-'; no leading/trailing '-';
 * truncated to cap-1. Returns bytes written (0 if token yields nothing). */
static size_t slugify_into(char *dst, size_t cap, const char *token) {
    size_t w            = 0u;
    bool   pending_dash = false;
    for (const char *p = token; *p != '\0' && w + 1u < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (c >= 'A' && c <= 'Z') {
            c = (unsigned char)(c - 'A' + 'a');
        }
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            if (pending_dash && w > 0u && w + 1u < cap) {
                dst[w++] = '-';
            }
            pending_dash = false;
            if (w + 1u < cap) {
                dst[w++] = (char)c;
            }
        } else {
            pending_dash = w > 0u; /* no leading dash */
        }
    }
    dst[w] = '\0';
    return w;
}

bool spg_reflect_outcome(const char *shape_token, const char *expected_substring,
                         const char *observation, struct spg_lesson *out) {
    if (out == nullptr || shape_token == nullptr || shape_token[0] == '\0' ||
        expected_substring == nullptr || expected_substring[0] == '\0' ||
        observation == nullptr) {
        return false;
    }

    /* slug = "lesson-outcome-<shape>", deduping per task shape. If the token
     * sanitizes to nothing (e.g. all punctuation), fall back so the slug stays
     * valid and stable rather than colliding on the bare prefix. */
    const size_t w =
        (size_t)snprintf(out->slug, sizeof out->slug, "%s", "lesson-outcome-");
    if (slugify_into(out->slug + w, sizeof out->slug - w, shape_token) == 0u) {
        (void)snprintf(out->slug + w, sizeof out->slug - w, "%s", "x");
    }

    (void)snprintf(out->description, sizeof out->description,
                   "Finish only when the output shows the expected result; "
                   "last run missed \"%.80s\".",
                   expected_substring);

    /* The concrete miss, verbatim — the fact, not an invented correction. */
    (void)snprintf(
        out->body, sizeof out->body,
        "A previous run finished but its output did not meet the success "
        "criterion. Expected the observation to contain: \"%.200s\". The "
        "observation was: \"%.400s\". Before emitting (recommend (kind finish) "
        "...), make sure the required result is actually present.",
        expected_substring, observation[0] != '\0' ? observation : "(empty)");
    return true;
}

bool spg_reflect_skill(const char *shape_token, const char *procedure_summary,
                       struct spg_lesson *out) {
    if (out == nullptr || shape_token == nullptr || shape_token[0] == '\0' ||
        procedure_summary == nullptr || procedure_summary[0] == '\0') {
        return false;
    }
    /* slug = "skill-<shape>": distinct namespace from lesson-*, deduped per
     * task shape so a later same-shape task recalls this one procedure. */
    const size_t w = (size_t)snprintf(out->slug, sizeof out->slug, "%s", "skill-");
    if (slugify_into(out->slug + w, sizeof out->slug - w, shape_token) == 0u) {
        (void)snprintf(out->slug + w, sizeof out->slug - w, "%s", "x");
    }
    (void)snprintf(out->description, sizeof out->description,
                   "For a %.60s task, a working approach: %.120s.", shape_token,
                   procedure_summary);
    (void)snprintf(
        out->body, sizeof out->body,
        "A task of shape \"%.120s\" was completed successfully with this "
        "action sequence: %.400s. Follow the same shape: emit each action as "
        "one valid (recommend ...) form and finish once the goal is met.",
        shape_token, procedure_summary);
    return true;
}
