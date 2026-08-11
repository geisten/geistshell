#ifndef GEISTSHELL_DEVICE_EXECUTOR_H
#define GEISTSHELL_DEVICE_EXECUTOR_H

#include "geistshell/device.h"
#include "geistshell/journal.h"
#include "geistshell/policy.h"
#include "geistshell/recommendation.h"
#include "geistshell/status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Executing SPG_ACTION_DEVICE_WRITE.
 *
 * Every way this can end is named. A single boolean "did it work" would put a
 * policy refusal, a machine that stopped answering and a channel nobody
 * declared into one bucket, and those call for three different responses from
 * whoever reads the journal. */
enum spg_device_outcome {
    SPG_DEVICE_OUTCOME_NOT_EXECUTED = 0, /* dry run, or not an allowed write */
    SPG_DEVICE_OUTCOME_WRITTEN,
    SPG_DEVICE_OUTCOME_NO_DEVICE,        /* no machine attached to this run */
    SPG_DEVICE_OUTCOME_UNKNOWN_CHANNEL,  /* not in the table */
    SPG_DEVICE_OUTCOME_REFUSED,          /* out of range, or read-only */
    SPG_DEVICE_OUTCOME_WATCHDOG_EXPIRED, /* contact lost; safe state driven */
    SPG_DEVICE_OUTCOME_IO_FAILED,
};

[[nodiscard]] const char *
spg_device_outcome_to_string(enum spg_device_outcome outcome);

struct spg_device_executor_state {
    /* Null when this run has no machine. Not an error: an agent that reasons
     * about a device it cannot reach still produces a journal, and the outcome
     * says so. */
    struct spg_device         *device;
    struct spg_journal_writer *journal;
};

struct spg_device_executor_config {
    uint64_t actor_id;
    /* Injected, never read from a clock — the watchdog deadline is judged
     * against this, so a replay reaches the same verdict. */
    uint64_t timestamp_ns;
    uint64_t parent_sequence;
    bool     write_journal;
    bool     execution_enabled;
};

struct spg_device_executor_result {
    enum spg_device_outcome outcome;
    char                    channel[SPG_DEVICE_NAME_CAP];
    int64_t                 value;
    enum spg_status         write_status;
    /* Set when the watchdog fired: whether the safe state was driven, and how
     * that itself went. A failed safe state is the worst case in this file and
     * must not be silent. */
    bool            safe_state_driven;
    enum spg_status safe_state_status;
    uint64_t        sequence;
};

[[nodiscard]] enum spg_status
spg_device_executor_step(const struct spg_device_executor_state  *state,
                         const struct spg_device_executor_config *config,
                         size_t output_n, const char output[],
                         const struct spg_recommendation   *recommendation,
                         const struct spg_policy_decision  *decision,
                         struct spg_device_executor_result *out);

#ifdef __cplusplus
}
#endif

#endif
