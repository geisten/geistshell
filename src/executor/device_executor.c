/* The executor for SPG_ACTION_DEVICE_WRITE.
 *
 * Separate from machine_executor.c because the two guard different things: one
 * signals a process on this host, the other moves something physical over a
 * network. The checks below are the "stronger story" the action enum asks for
 * before an irreversible action is allowed at all. */

#include "geistshell/device_executor.h"

#include "geistshell/journal.h"

#include <stdio.h>
#include <string.h>

const char *
spg_device_outcome_to_string(const enum spg_device_outcome outcome) {
    switch (outcome) {
    case SPG_DEVICE_OUTCOME_WRITTEN:
        return "written";
    case SPG_DEVICE_OUTCOME_NO_DEVICE:
        return "no_device";
    case SPG_DEVICE_OUTCOME_UNKNOWN_CHANNEL:
        return "unknown_channel";
    case SPG_DEVICE_OUTCOME_REFUSED:
        return "refused";
    case SPG_DEVICE_OUTCOME_WATCHDOG_EXPIRED:
        return "watchdog_expired";
    case SPG_DEVICE_OUTCOME_IO_FAILED:
        return "io_failed";
    case SPG_DEVICE_OUTCOME_NOT_EXECUTED:
        return "not_executed";
    }
    return "unknown";
}

/* Copy a span out of the model output, refusing anything that does not fit.
 * The span is attacker-adjacent data — it is whatever the model emitted — so
 * its bounds are checked against the buffer it claims to point into. */
static enum spg_status copy_span(const size_t output_n, const char output[],
                                 const struct spg_text_span span,
                                 const size_t cap, char out[]) {
    if (span.length == 0u || span.length + 1u > cap ||
        span.offset + span.length > output_n) {
        return SPG_E_SCHEMA;
    }
    memcpy(out, &output[span.offset], span.length);
    out[span.length] = '\0';
    return SPG_OK;
}

enum spg_status
spg_device_executor_step(const struct spg_device_executor_state  *state,
                         const struct spg_device_executor_config *config,
                         const size_t output_n, const char output[],
                         const struct spg_recommendation   *recommendation,
                         const struct spg_policy_decision  *decision,
                         struct spg_device_executor_result *out) {
    if (state == nullptr || config == nullptr || recommendation == nullptr ||
        decision == nullptr || out == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out = (struct spg_device_executor_result){
        .outcome = SPG_DEVICE_OUTCOME_NOT_EXECUTED};

    if (recommendation->action_kind != SPG_ACTION_DEVICE_WRITE ||
        decision->kind != SPG_POLICY_DECISION_ALLOW) {
        return SPG_OK;
    }

    enum spg_status status = copy_span(output_n, output, recommendation->target,
                                       sizeof out->channel, out->channel);
    if (status != SPG_OK) {
        return status;
    }
    out->value = recommendation->device_value;

    if (state->device == nullptr) {
        /* No machine is attached to this run. Reported as its own outcome
         * rather than as a refusal: "there is nothing to write to" and "the
         * machine said no" are different facts, and an operator reading the
         * journal must not have to guess which one happened. */
        out->outcome = SPG_DEVICE_OUTCOME_NO_DEVICE;
    } else if (spg_device_find(state->device, out->channel) == nullptr) {
        out->outcome = SPG_DEVICE_OUTCOME_UNKNOWN_CHANNEL;
    } else if (spg_device_watchdog_check(state->device, config->timestamp_ns) ==
               SPG_WATCHDOG_EXPIRED) {
        /* Checked BEFORE the write, not after. A machine that has stopped
         * answering is a machine whose state is unknown, and the one command
         * that must still be attempted on it is the safe state — not the next
         * setpoint a model happened to pick. */
        out->outcome           = SPG_DEVICE_OUTCOME_WATCHDOG_EXPIRED;
        out->safe_state_status = spg_device_safe_state(state->device);
        out->safe_state_driven = true;
    } else if (!config->execution_enabled) {
        out->outcome = SPG_DEVICE_OUTCOME_NOT_EXECUTED;
    } else {
        const enum spg_status write_status =
            spg_device_write(state->device, out->channel, out->value);
        if (write_status == SPG_OK) {
            out->outcome = SPG_DEVICE_OUTCOME_WRITTEN;
        } else if (write_status == SPG_E_LIMIT ||
                   write_status == SPG_E_POLICY_DENIED ||
                   write_status == SPG_E_OVERFLOW) {
            /* The channel table said no. This is the bound that makes an
             * irreversible action tolerable, so it is recorded as a refusal
             * rather than folded into a generic I/O failure. */
            out->outcome = SPG_DEVICE_OUTCOME_REFUSED;
        } else {
            out->outcome = SPG_DEVICE_OUTCOME_IO_FAILED;
        }
        out->write_status = write_status;
    }

    if (!config->write_journal || state->journal == nullptr) {
        return SPG_OK;
    }
    /* Journalled whatever the outcome, including the refusals. A refused write
     * that leaves no record is a refusal nobody can audit — and on an
     * irreversible action the record of what was NOT done matters as much as
     * the record of what was.
     *
     * Reuses SPG_JOURNAL_EVENT_ACTION rather than adding an event kind: the
     * payload already names itself, and a new kind would have to be taught to
     * the replay tool and every frozen journal fixture for no gain. */
    char      record[512] = {};
    const int written     = snprintf(
        record, sizeof record,
        "(device (channel \"%s\") (value %lld) (outcome %s) (executed %s))",
        out->channel, (long long)out->value,
        spg_device_outcome_to_string(out->outcome),
        out->outcome == SPG_DEVICE_OUTCOME_WRITTEN ? "true" : "false");
    if (written < 0 || (size_t)written >= sizeof record) {
        return SPG_E_LIMIT;
    }
    uint64_t              sequence = 0u;
    const enum spg_status status_j = spg_journal_writer_append(
        state->journal, config->timestamp_ns, config->parent_sequence,
        SPG_JOURNAL_EVENT_ACTION,
        out->outcome == SPG_DEVICE_OUTCOME_WRITTEN ? SPG_OK
                                                   : SPG_E_INVALID_STATE,
        (size_t)written, (const uint8_t *)record, &sequence);
    out->sequence = sequence;
    return status_j;
}
