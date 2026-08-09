#ifndef GEISTSHELL_PROCESS_PROFILE_H
#define GEISTSHELL_PROCESS_PROFILE_H

#include "geistshell/machine_state.h"
#include "geistshell/sexpr.h"
#include "geistshell/status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Configuration that gives processes a semantic role (roadmap phase 2, #62):
 *
 *   (process-profile
 *     (process "critical_app"
 *       (match "critical-worker")
 *       (role critical)
 *       (may_pause false)
 *       (may_stop false))
 *     (process "batch_job"
 *       (match "batch-worker")
 *       (role batch)
 *       (may_pause true)
 *       (may_stop true)))
 *
 * Separate header from machine_state.h on purpose: this is configuration read
 * once, that is a sample taken every tick.
 *
 * The strings are copied into fixed buffers rather than kept as spans into the
 * config text (which is what policy_config does): a profile outlives the buffer
 * it was parsed from and is compared against process names every tick. */

struct spg_process_profile_error {
    enum spg_status status;
    uint32_t        node_index;
    size_t          offset;
};

/* Parse a (process-profile ...) form. An absent or empty profile is valid —
 * it simply means nothing is managed. Duplicate ids are rejected
 * (SPG_E_SCHEMA): two entries with the same id would make phase 6's action
 * target ambiguous. */
[[nodiscard]] enum spg_status spg_process_profile_load(
    size_t input_n, const char input[], size_t token_capacity,
    struct spg_sexpr_token tokens[static token_capacity], size_t node_capacity,
    struct spg_sexpr_node       nodes[static node_capacity],
    struct spg_process_profile *out, struct spg_process_profile_error *error);

/* Index of the first entry matching name, or SPG_PROCESS_NO_PROFILE.
 *
 * Matching is exact against the kernel comm, with one concession to reality:
 * the kernel truncates comm to 15 characters, so a match string longer than
 * that also matches when its first 15 characters equal comm. First entry
 * wins — order in the file is the tie-break, and it is deliberate. */
[[nodiscard]] uint32_t
spg_process_profile_match(const struct spg_process_profile *profile,
                          const char                       *name);

/* Stamp role, may_pause, may_stop and profile_index onto each sample.
 * Unmatched processes keep role unknown and both permissions false — an
 * unknown process is never implicitly pausable. */
void spg_process_apply_profile(const struct spg_process_profile *profile,
                               size_t n, struct spg_process_sample procs[]);

#ifdef __cplusplus
}
#endif

#endif
