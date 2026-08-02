#ifndef GEISTSHELL_MEM_EXECUTOR_H
#define GEISTSHELL_MEM_EXECUTOR_H

#include "geistshell/journal.h"
#include "geistshell/mem_store.h"
#include "geistshell/policy.h"
#include "geistshell/recommendation.h"
#include "geistshell/status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Executes an ALLOW'd memory_save recommendation: persists it to the long-term
 * store and journals one SPG_JOURNAL_EVENT_MEMORY event so replay can
 * reconstruct the materialized store. Mirrors spg_sim_executor_step. */

struct spg_mem_executor_state {
    struct spg_mem_store      *store;
    struct spg_journal_writer *journal;
};

struct spg_mem_executor_config {
    uint32_t actor_id;
    uint64_t timestamp_ns;
    uint64_t parent_sequence;
    bool     write_journal;
};

struct spg_mem_executor_workspace {
    size_t payload_capacity; /* journal-event s-expression buffer */
    char  *payload;
    /* For memory_read: the read content is written here (NUL-terminated). May
     * be null/0 for save/delete. */
    size_t recall_capacity;
    char  *recall;
};

struct spg_mem_executor_result {
    enum spg_status save_status; /* outcome of the store operation (SPG_OK on
                                    success) */
    bool            was_read;    /* this was a memory_read */
    size_t          read_len;    /* bytes available for a read (full length) */
    uint64_t        memory_sequence;
    size_t          payload_used;
    bool            payload_truncated;
};

/* Executes an ALLOW'd memory_save / memory_delete / memory_read recommendation
 * against the store and journals one SPG_JOURNAL_EVENT_MEMORY event.
 * text/text_n is the recommendation source (the model output) that the
 * recommendation's mem_* spans index into. Returns SPG_E_INVALID_ARG on bad
 * arguments; otherwise SPG_OK with the outcome reported in *result (a failed
 * store op is recorded, not propagated as a step error). */
[[nodiscard]] enum spg_status spg_mem_executor_step(
    struct spg_mem_executor_state *state,
    const struct spg_mem_executor_config *config, size_t text_n,
    const char text[], const struct spg_recommendation *recommendation,
    const struct spg_policy_decision *policy_decision,
    const struct spg_mem_executor_workspace *workspace,
    struct spg_mem_executor_result *result);

#ifdef __cplusplus
}
#endif

#endif
