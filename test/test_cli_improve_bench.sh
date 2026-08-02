#!/bin/sh
# System test: the eval-gated self-improvement benchmark runs, lifts the held-out
# pass count, reverts the harmful lesson, and keeps no regression. Thin wrapper
# around the reproducible benchmark so CI guards the numbers in the README.
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}
export SPG_BIN

OUT=$(sh examples/eval/bench/improve_benchmark.sh)
printf '%s\n' "$OUT"
printf '%s\n' "$OUT" | grep -q 'held-out pass   : 3/5  ->  5/5'
printf '%s\n' "$OUT" | grep -q 'lessons         : 2 proposed, 1 kept, 1 reverted'
printf '%s\n' "$OUT" | grep -q 'benchmark: PASS'

echo "test_cli_improve_bench: PASS"
