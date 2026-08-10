#ifndef GEISTSHELL_FIXTURE_H
#define GEISTSHELL_FIXTURE_H

#include "geistshell/status.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Per-sample fixture isolation for the eval harness (geistshell#52).
 *
 * A stateful eval case mutates what it runs against: it writes a file into the
 * workdir, or saves a memory. Run it twice against one directory and the second
 * run finds the mutation already there and passes trivially — without ever
 * performing the action under test. With `--samples N` that is N-1 fabricated
 * successes, which is worse than no measurement because it looks like one.
 *
 * The fix is a directory per sample, reset from a pristine fixture before the
 * run. This module owns exactly that, split so the dangerous part is small,
 * separately testable, and cannot be handed an arbitrary path.
 *
 * ponytail: the copy and the delete shell out to cp(1) and rm(1) via
 * posix_spawn rather than walking the tree in C — same call already made for
 * curl in model_resolve.c. If eval ever needs to run where those are absent,
 * the replacement is a recursive opendir/readdir copy behind these same three
 * functions. */

/* Path of the sandbox for one (case, sample): "<root>/<case_name>-<sample>".
 *
 * PURE — touches no filesystem. It is also the only sanctioned way to produce a
 * path for spg_fixture_reset, because it is where case_name is validated:
 * a name is rejected unless every byte is alphanumeric, '.', '_' or '-'. That
 * bans '/' and ".." at the source, so a suite file cannot steer a delete out of
 * the build tree.
 *
 * Returns SPG_E_INVALID_ARG on a null/empty argument or a rejected name,
 * SPG_E_LIMIT if the path does not fit in cap. */
[[nodiscard]] enum spg_status spg_fixture_sample_dir(const char *root,
                                                     const char *case_name,
                                                     size_t      sample,
                                                     size_t      cap,
                                                     char        out[]);

/* Delete `dir` and recreate it empty.
 *
 * This is the one destructive call in the harness, so it validates rather than
 * trusts, even though its only in-tree caller passes a path from
 * spg_fixture_sample_dir. `dir` is rejected unless it is relative (no leading
 * '/'), contains no ".." component, and is nested at least one level deep — so
 * "." , "..", "/", "build" and "../x" can never be the target. A path that
 * fails any of these returns SPG_E_INVALID_ARG and nothing is removed.
 *
 * Returns SPG_E_IO if the delete or the recreate fails. */
[[nodiscard]] enum spg_status spg_fixture_reset(const char *dir);

/* Copy the CONTENTS of src_dir into dst_dir (not src_dir itself), so calling it
 * twice with different sources overlays them in call order. Both must exist.
 * A null or empty src_dir is a no-op returning SPG_OK — "this case has no
 * fixture" is not an error.
 *
 * Returns SPG_E_NOT_FOUND if src_dir is missing, SPG_E_IO on a copy failure. */
[[nodiscard]] enum spg_status spg_fixture_copy_into(const char *dst_dir,
                                                    const char *src_dir);

#ifdef __cplusplus
}
#endif

#endif
