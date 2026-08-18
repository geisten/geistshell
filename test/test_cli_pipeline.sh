#!/bin/sh
# System test: the full `run` pipeline (recommendation -> policy -> sim ->
# journal -> state), journal verification, replay, and run-to-run determinism.
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

REC='(recommend (kind simulator) (capability "sim.act") (cost 1) (uses_network false) (confidence_bp 9000) (reason "reduce risk"))'

cat > "$T/run.spg" <<EOF
(run
 (model "fake.gguf")
 (policy "examples/policy.spg")
 (scenario "examples/scenario.spg")
 (corpus "examples/corpus.spg")
 (journal "$T/j.sgj")
 (seed 42)
 (budgets (inference_steps 8) (tokens 256) (shell_actions 1) (sim_actions 8) (wall_ms 10000)))
EOF

# --- multi-tick run with state outputs ---
"$SPG_BIN" run --config "$T/run.spg" --fake "$REC" --ticks 3 \
    --write-sim-state "$T/final.spg" --write-run-state "$T/state.json" \
    > "$T/run.out" 2>&1
grep -q "ticks=3" "$T/run.out"

# run-state JSON reflects three simulator actions and accumulated graph/memory
grep -q '"ticks":3' "$T/state.json"
grep -q '"sim_actions":3' "$T/state.json"
grep -q '"facts":6' "$T/state.json"
grep -q '"nodes":9' "$T/state.json"

# --- journal verifies: hash chain intact, no failed events ---
V=$("$SPG_BIN" verify-journal "$T/j.sgj")
echo "$V" | grep -q "verified=true"
echo "$V" | grep -q "status_failures=0"
echo "$V" | grep -q "event.policy_decision=3"

# --- replay reproduces the event stream and is byte-deterministic ---
test "$("$SPG_BIN" replay "$T/j.sgj" | wc -l | tr -d ' ')" = "24"
"$SPG_BIN" replay "$T/j.sgj" > "$T/r1.jsonl"
"$SPG_BIN" replay "$T/j.sgj" > "$T/r2.jsonl"
cmp "$T/r1.jsonl" "$T/r2.jsonl"

# --- two runs produce byte-identical journals (logical timestamps) ---
cp "$T/j.sgj" "$T/j_first.sgj"
rm "$T/j.sgj"
"$SPG_BIN" run --config "$T/run.spg" --fake "$REC" --ticks 3 >/dev/null 2>&1
cmp "$T/j_first.sgj" "$T/j.sgj"

# --- the written final scenario is itself valid input ---
"$SPG_BIN" sim-validate "$T/final.spg" | grep -q "valid=true"

echo "test_cli_pipeline: PASS"
