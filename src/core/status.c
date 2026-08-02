#include "geistshell/status.h"

const char *spg_status_to_string(const enum spg_status status) {
    switch (status) {
    case SPG_OK:
        return "SPG_OK";
    case SPG_E_OOM:
        return "SPG_E_OOM";
    case SPG_E_INVALID_ARG:
        return "SPG_E_INVALID_ARG";
    case SPG_E_INVALID_STATE:
        return "SPG_E_INVALID_STATE";
    case SPG_E_NOT_FOUND:
        return "SPG_E_NOT_FOUND";
    case SPG_E_OVERFLOW:
        return "SPG_E_OVERFLOW";
    case SPG_E_LIMIT:
        return "SPG_E_LIMIT";
    case SPG_E_IO:
        return "SPG_E_IO";
    case SPG_E_FORMAT:
        return "SPG_E_FORMAT";
    case SPG_E_SCHEMA:
        return "SPG_E_SCHEMA";
    case SPG_E_POLICY_DENIED:
        return "SPG_E_POLICY_DENIED";
    case SPG_E_BUDGET_EXCEEDED:
        return "SPG_E_BUDGET_EXCEEDED";
    case SPG_E_ATTESTATION:
        return "SPG_E_ATTESTATION";
    case SPG_E_JOURNAL_CORRUPT:
        return "SPG_E_JOURNAL_CORRUPT";
    case SPG_E_REPLAY_MISMATCH:
        return "SPG_E_REPLAY_MISMATCH";
    case SPG_E_MODEL:
        return "SPG_E_MODEL";
    case SPG_E_UNSUPPORTED:
        return "SPG_E_UNSUPPORTED";
    case SPG_E_INTERNAL:
        return "SPG_E_INTERNAL";
    default:
        return "SPG_E_UNKNOWN";
    }
}
