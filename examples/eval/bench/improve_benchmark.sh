#!/bin/sh
# Reproducible benchmark for the eval-gated self-improvement loop.
#
# It distils candidate lessons from a TRAIN suite and judges them on a held-out
# VALIDATION suite (cases the lessons were not derived from). It then reports the
# held-out pass count before vs. after learning, how many candidate lessons were
# proposed / kept / reverted, and asserts the governance invariant:
#
#     held-out final >= held-out baseline   (no net regression is ever kept)
#
# The models are deterministic scripted fakes, so the numbers are byte-identical
# on every run and in CI. With a real or remote model, run the same command with
# `--samples N`: each case is run N times, the per-case verdict reports `k of N`,
# and the gate compares the *summed* pass counts (variance handling). Those runs
# are non-deterministic in *which* sample you get, but each one stays
# journal-replayable byte-for-byte.
#
# Usage: examples/eval/bench/improve_benchmark.sh   (run from the repo root)
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}
TRAIN=examples/eval/bench/train.spg
HOLDOUT=examples/eval/bench/holdout.spg
MEM=$(mktemp -d)
trap 'rm -rf "$MEM"' EXIT

OUT=$("$SPG_BIN" improve "$TRAIN" --validate "$HOLDOUT" --memory-dir "$MEM")

# Total held-out cases (denominator) = number of (case ...) forms in the suite.
TOTAL=$(grep -c '(case ' "$HOLDOUT")
BASE=$(printf '%s\n' "$OUT"  | sed -n 's/.*"held_out_baseline":\([0-9]*\).*/\1/p')
FINAL=$(printf '%s\n' "$OUT" | sed -n 's/.*"held_out_final":\([0-9]*\).*/\1/p')
KEPT=$(printf '%s\n' "$OUT"  | sed -n 's/.*"lessons_kept":\([0-9]*\).*/\1/p')
PROPOSED=$(printf '%s\n' "$OUT" | grep -c '"lesson":')
REVERTED=$(printf '%s\n' "$OUT" | grep -c '"accepted":false')

printf '\n=== eval-gated self-improvement benchmark =====================\n'
printf 'train suite     : %s\n' "$TRAIN"
printf 'held-out suite  : %s  (%s cases)\n' "$HOLDOUT" "$TOTAL"
printf -- '---------------------------------------------------------------\n'
printf 'held-out pass   : %s/%s  ->  %s/%s   (baseline -> after learning)\n' \
       "$BASE" "$TOTAL" "$FINAL" "$TOTAL"
printf 'lessons         : %s proposed, %s kept, %s reverted\n' \
       "$PROPOSED" "$KEPT" "$REVERTED"
printf 'regressions kept: 0  (invariant: final >= baseline)\n'
printf -- '---------------------------------------------------------------\n'

# Assertions: the lift is real and the safety invariant holds.
fail=0
[ "$FINAL" -ge "$BASE" ] || { echo "FAIL: held-out regressed ($FINAL < $BASE)"; fail=1; }
[ "$FINAL" -gt "$BASE" ] || { echo "FAIL: no measurable lift"; fail=1; }
[ "$REVERTED" -ge 1 ]    || { echo "FAIL: expected a harmful lesson to be reverted"; fail=1; }
[ "$fail" -eq 0 ] || exit 1

printf 'benchmark: PASS\n\n'
