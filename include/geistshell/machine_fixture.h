#ifndef GEISTSHELL_MACHINE_FIXTURE_H
#define GEISTSHELL_MACHINE_FIXTURE_H

#include "geistshell/machine_state.h"
#include "geistshell/sexpr.h"
#include "geistshell/status.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read a (machine-state ...) block into a snapshot — the inverse of
 * spg_machine_state_render, in the same file-loading shape as the policy and
 * profile loaders.
 *
 * A diagnosis scenario (roadmap phase 4, #64) IS a machine state, and an eval
 * case must reproduce on a host with no Pi attached: sampling the machine would
 * make every case depend on what happens to be running. So scenarios are
 * fixtures, and this reads them.
 *
 * Absent fields and an explicit `unknown` both yield the unknown sentinel,
 * never 0. An unrecognised role is rejected (SPG_E_SCHEMA) rather than
 * downgraded, so a typo cannot strip a critical process of its protection.
 * More than SPG_MACHINE_MAX_PROCESSES process entries yields SPG_E_LIMIT. */
[[nodiscard]] enum spg_status spg_machine_state_parse(
    size_t input_n, const char input[], size_t token_capacity,
    struct spg_sexpr_token tokens[static token_capacity], size_t node_capacity,
    struct spg_sexpr_node    nodes[static node_capacity],
    struct spg_machine_state *out);

#ifdef __cplusplus
}
#endif

#endif
