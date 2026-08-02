#include "geistshell/recommendation.h"

#include <stdio.h>
#include <string.h>

static enum spg_status parse_text(const char *text,
                                  struct spg_recommendation *out,
                                  struct spg_recommendation_error *error) {
    struct spg_sexpr_token tokens[128];
    struct spg_sexpr_node  nodes[128];
    return spg_recommendation_parse(strlen(text), text, 128u, tokens, 128u,
                                    nodes, out, error);
}

static int test_valid_simulator(void) {
    const char text[] =
        "(recommend (kind simulator) (capability \"sim\") (cost 1) "
        "(uses_network false) (confidence_bp 6500) "
        "(reason \"low certainty\"))";
    struct spg_recommendation       rec = {};
    struct spg_recommendation_error err = {};
    if (parse_text(text, &rec, &err) != SPG_OK) {
        return 1;
    }
    if (rec.state != SPG_RECOMMENDATION_VALID ||
        rec.action_kind != SPG_ACTION_SIMULATOR ||
        rec.action.kind != SPG_ACTION_SIMULATOR ||
        rec.action.uses_network || rec.action.cost != 1u ||
        rec.confidence_bp != 6500u) {
        return 1;
    }
    return rec.capability.length == 3u &&
                   memcmp(text + rec.capability.offset, "sim", 3u) == 0
               ? 0
               : 1;
}

static int test_valid_local_shell(void) {
    const char text[] =
        "(recommend (kind local_shell) (capability \"local\") (cost 1) "
        "(uses_network false) (confidence_bp 5000) "
        "(reason \"inspect local state\") (command \"uname -a\"))";
    struct spg_recommendation       rec = {};
    struct spg_recommendation_error err = {};
    if (parse_text(text, &rec, &err) != SPG_OK) {
        return 1;
    }
    if (rec.state != SPG_RECOMMENDATION_VALID ||
        rec.action_kind != SPG_ACTION_LOCAL_SHELL || !rec.has_command ||
        rec.has_target || rec.command.length != strlen("uname -a")) {
        return 1;
    }
    return 0;
}

static int test_valid_ssh_probe(void) {
    const char text[] =
        "(recommend (kind ssh_auth_probe) (capability \"ssh\") (cost 1) "
        "(uses_network true) (confidence_bp 3000) "
        "(reason \"attested host only\") (target \"lab-host\"))";
    struct spg_recommendation       rec = {};
    struct spg_recommendation_error err = {};
    if (parse_text(text, &rec, &err) != SPG_OK) {
        return 1;
    }
    return rec.state == SPG_RECOMMENDATION_VALID &&
                   rec.action_kind == SPG_ACTION_SSH_AUTH_PROBE &&
                   rec.has_target && !rec.has_command &&
                   rec.action.uses_network
               ? 0
               : 1;
}

static int test_free_text_rejected(void) {
    struct spg_recommendation       rec = {};
    struct spg_recommendation_error err = {};
    if (parse_text("run whoami", &rec, &err) != SPG_OK) {
        return 1;
    }
    return rec.state == SPG_RECOMMENDATION_REJECTED &&
                   rec.reject_reason == SPG_RECOMMENDATION_REJECT_SCHEMA
               ? 0
               : 1;
}

static int test_syntax_rejected_without_error_return(void) {
    struct spg_recommendation       rec = {};
    struct spg_recommendation_error err = {};
    if (parse_text("(recommend (kind simulator)", &rec, &err) != SPG_OK) {
        return 1;
    }
    return rec.state == SPG_RECOMMENDATION_REJECTED &&
                   rec.reject_reason == SPG_RECOMMENDATION_REJECT_SYNTAX
               ? 0
               : 1;
}

static int test_missing_required(void) {
    const char text[] =
        "(recommend (kind simulator) (capability \"sim\") (cost 1) "
        "(uses_network false) (reason \"x\"))";
    struct spg_recommendation       rec = {};
    struct spg_recommendation_error err = {};
    if (parse_text(text, &rec, &err) != SPG_OK) {
        return 1;
    }
    return rec.reject_reason == SPG_RECOMMENDATION_REJECT_MISSING_FIELD ? 0
                                                                        : 1;
}

static int test_duplicate_field(void) {
    const char text[] =
        "(recommend (kind simulator) (kind simulator) (capability \"sim\") "
        "(cost 1) (uses_network false) (confidence_bp 1) (reason \"x\"))";
    struct spg_recommendation       rec = {};
    struct spg_recommendation_error err = {};
    if (parse_text(text, &rec, &err) != SPG_OK) {
        return 1;
    }
    return rec.reject_reason == SPG_RECOMMENDATION_REJECT_DUPLICATE_FIELD ? 0
                                                                          : 1;
}

static int test_unknown_kind(void) {
    const char text[] =
        "(recommend (kind exploit) (capability \"x\") (cost 1) "
        "(uses_network false) (confidence_bp 1) (reason \"x\"))";
    struct spg_recommendation       rec = {};
    struct spg_recommendation_error err = {};
    if (parse_text(text, &rec, &err) != SPG_OK) {
        return 1;
    }
    return rec.reject_reason == SPG_RECOMMENDATION_REJECT_UNKNOWN_KIND ? 0 : 1;
}

static int test_wrong_value(void) {
    const char text[] =
        "(recommend (kind simulator) (capability sim) (cost 0) "
        "(uses_network false) (confidence_bp 10001) (reason \"x\"))";
    struct spg_recommendation       rec = {};
    struct spg_recommendation_error err = {};
    if (parse_text(text, &rec, &err) != SPG_OK) {
        return 1;
    }
    return rec.reject_reason == SPG_RECOMMENDATION_REJECT_WRONG_VALUE ? 0 : 1;
}

static int test_kind_mismatch(void) {
    const char text[] =
        "(recommend (kind local_shell) (capability \"local\") (cost 1) "
        "(uses_network true) (confidence_bp 7000) (reason \"x\") "
        "(command \"id\"))";
    struct spg_recommendation       rec = {};
    struct spg_recommendation_error err = {};
    if (parse_text(text, &rec, &err) != SPG_OK) {
        return 1;
    }
    return rec.reject_reason == SPG_RECOMMENDATION_REJECT_KIND_MISMATCH ? 0
                                                                        : 1;
}

static int test_capacity_limit_returns_limit(void) {
    const char text[] =
        "(recommend (kind simulator) (capability \"sim\") (cost 1) "
        "(uses_network false) (confidence_bp 1) (reason \"x\"))";
    struct spg_sexpr_token          tokens[2];
    struct spg_sexpr_node           nodes[2];
    struct spg_recommendation       rec = {};
    struct spg_recommendation_error err = {};
    return spg_recommendation_parse(strlen(text), text, 2u, tokens, 2u, nodes,
                                    &rec, &err) == SPG_E_LIMIT
               ? 0
               : 1;
}

static int test_invalid_args(void) {
    struct spg_sexpr_token          tokens[1];
    struct spg_sexpr_node           nodes[1];
    struct spg_recommendation       rec = {};
    struct spg_recommendation_error err = {};
    if (spg_recommendation_parse(0u, nullptr, 1u, tokens, 1u, nodes, &rec,
                                 &err) != SPG_E_INVALID_ARG) {
        return 1;
    }
    return spg_recommendation_reject_reason_to_string(
               SPG_RECOMMENDATION_REJECT_KIND_MISMATCH) != nullptr
               ? 0
               : 1;
}

static int test_valid_finish(void) {
    /* finish needs only kind + reason -- no capability/cost/network. */
    const char text[] = "(recommend (kind finish) (reason \"task done\"))";
    struct spg_recommendation       rec = {};
    struct spg_recommendation_error err = {};
    if (parse_text(text, &rec, &err) != SPG_OK) {
        return 1;
    }
    return (rec.state == SPG_RECOMMENDATION_VALID &&
            rec.action_kind == SPG_ACTION_FINISH && !rec.has_command &&
            !rec.has_target && !rec.has_slug)
               ? 0
               : 1;
}

static int test_finish_with_command_rejected(void) {
    /* finish must not carry side-effect fields. */
    const char text[] =
        "(recommend (kind finish) (reason \"x\") (command \"rm -rf /\"))";
    struct spg_recommendation       rec = {};
    struct spg_recommendation_error err = {};
    if (parse_text(text, &rec, &err) != SPG_OK) {
        return 1;
    }
    return (rec.state == SPG_RECOMMENDATION_REJECTED &&
            rec.reject_reason == SPG_RECOMMENDATION_REJECT_KIND_MISMATCH)
               ? 0
               : 1;
}

static int test_finish_missing_reason_rejected(void) {
    const char text[] = "(recommend (kind finish))";
    struct spg_recommendation       rec = {};
    struct spg_recommendation_error err = {};
    if (parse_text(text, &rec, &err) != SPG_OK) {
        return 1;
    }
    return (rec.state == SPG_RECOMMENDATION_REJECTED &&
            rec.reject_reason == SPG_RECOMMENDATION_REJECT_MISSING_FIELD)
               ? 0
               : 1;
}

int main(void) {
    if (test_valid_finish() != 0) {
        fprintf(stderr, "test_valid_finish failed\n");
        return 1;
    }
    if (test_finish_with_command_rejected() != 0) {
        fprintf(stderr, "test_finish_with_command_rejected failed\n");
        return 1;
    }
    if (test_finish_missing_reason_rejected() != 0) {
        fprintf(stderr, "test_finish_missing_reason_rejected failed\n");
        return 1;
    }
    if (test_valid_simulator() != 0) {
        fprintf(stderr, "test_valid_simulator failed\n");
        return 1;
    }
    if (test_valid_local_shell() != 0) {
        fprintf(stderr, "test_valid_local_shell failed\n");
        return 1;
    }
    if (test_valid_ssh_probe() != 0) {
        fprintf(stderr, "test_valid_ssh_probe failed\n");
        return 1;
    }
    if (test_free_text_rejected() != 0) {
        fprintf(stderr, "test_free_text_rejected failed\n");
        return 1;
    }
    if (test_syntax_rejected_without_error_return() != 0) {
        fprintf(stderr, "test_syntax_rejected_without_error_return failed\n");
        return 1;
    }
    if (test_missing_required() != 0) {
        fprintf(stderr, "test_missing_required failed\n");
        return 1;
    }
    if (test_duplicate_field() != 0) {
        fprintf(stderr, "test_duplicate_field failed\n");
        return 1;
    }
    if (test_unknown_kind() != 0) {
        fprintf(stderr, "test_unknown_kind failed\n");
        return 1;
    }
    if (test_wrong_value() != 0) {
        fprintf(stderr, "test_wrong_value failed\n");
        return 1;
    }
    if (test_kind_mismatch() != 0) {
        fprintf(stderr, "test_kind_mismatch failed\n");
        return 1;
    }
    if (test_capacity_limit_returns_limit() != 0) {
        fprintf(stderr, "test_capacity_limit_returns_limit failed\n");
        return 1;
    }
    if (test_invalid_args() != 0) {
        fprintf(stderr, "test_invalid_args failed\n");
        return 1;
    }
    return 0;
}
