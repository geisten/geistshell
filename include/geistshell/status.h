#ifndef GEISTSHELL_STATUS_H
#define GEISTSHELL_STATUS_H

#ifdef __cplusplus
extern "C" {
#endif

enum spg_status {
    SPG_OK = 0,

    SPG_E_OOM,
    SPG_E_INVALID_ARG,
    SPG_E_INVALID_STATE,
    SPG_E_NOT_FOUND,
    SPG_E_OVERFLOW,
    SPG_E_LIMIT,

    SPG_E_IO,
    SPG_E_FORMAT,
    SPG_E_SCHEMA,

    SPG_E_POLICY_DENIED,
    SPG_E_BUDGET_EXCEEDED,
    SPG_E_ATTESTATION,

    SPG_E_JOURNAL_CORRUPT,
    SPG_E_REPLAY_MISMATCH,

    SPG_E_MODEL,
    SPG_E_UNSUPPORTED,
    SPG_E_INTERNAL,
};

[[nodiscard]] const char *spg_status_to_string(enum spg_status status);

#ifdef __cplusplus
}
#endif

#endif
