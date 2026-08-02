#ifndef GEISTSHELL_RECOMMENDATION_H
#define GEISTSHELL_RECOMMENDATION_H

#include "geistshell/policy.h"
#include "geistshell/sexpr.h"
#include "geistshell/status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum spg_recommendation_state {
    SPG_RECOMMENDATION_REJECTED = 0,
    SPG_RECOMMENDATION_VALID,
};

enum spg_recommendation_reject_reason {
    SPG_RECOMMENDATION_REJECT_NONE = 0,
    SPG_RECOMMENDATION_REJECT_EMPTY,
    SPG_RECOMMENDATION_REJECT_SYNTAX,
    SPG_RECOMMENDATION_REJECT_SCHEMA,
    SPG_RECOMMENDATION_REJECT_UNKNOWN_KIND,
    SPG_RECOMMENDATION_REJECT_MISSING_FIELD,
    SPG_RECOMMENDATION_REJECT_DUPLICATE_FIELD,
    SPG_RECOMMENDATION_REJECT_WRONG_VALUE,
    SPG_RECOMMENDATION_REJECT_KIND_MISMATCH,
};

struct spg_recommendation {
    enum spg_recommendation_state         state;
    enum spg_recommendation_reject_reason reject_reason;

    enum spg_action_kind action_kind;
    struct spg_action_request action;

    struct spg_text_span kind;
    struct spg_text_span capability;
    struct spg_text_span command;
    struct spg_text_span target;
    struct spg_text_span reason;

    /* memory_save fields (spans into the recommendation text). */
    struct spg_text_span mem_slug;
    struct spg_text_span mem_description;
    struct spg_text_span mem_body;

    uint64_t confidence_bp;
    bool     has_command;
    bool     has_target;
    bool     has_slug;
    bool     has_description;
    bool     has_body;
};

struct spg_recommendation_error {
    enum spg_status                       status;
    enum spg_recommendation_reject_reason reject_reason;
    uint32_t                              node_index;
    size_t                                offset;
};

[[nodiscard]] enum spg_status spg_recommendation_parse(
    size_t input_n, const char input[], size_t token_capacity,
    struct spg_sexpr_token tokens[static token_capacity], size_t node_capacity,
    struct spg_sexpr_node nodes[static node_capacity],
    struct spg_recommendation *out, struct spg_recommendation_error *error);

[[nodiscard]] const char *spg_recommendation_reject_reason_to_string(
    enum spg_recommendation_reject_reason reason);

#ifdef __cplusplus
}
#endif

#endif
