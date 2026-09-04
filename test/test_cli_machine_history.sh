#!/bin/sh
# Phase 3b (#79): the bounded history window, end to end. The scripted paths
# only — the rising-vs-falling behaviour probe needs a real model and lives in
# examples/eval/machine/history_probe.spg.
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

fail() {
    echo "test_cli_machine_history: FAIL — $1" >&2
    exit 1
}

# --- the agent renders the window into the journaled context ----------------
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
cat > "$T/script.txt" <<'EOF'
(recommend (kind simulator) (capability "sim.act") (cost 1) (uses_network false) (confidence_bp 7000) (reason "step one"))
(recommend (kind simulator) (capability "sim.act") (cost 1) (uses_network false) (confidence_bp 7000) (reason "step two"))
(recommend (kind finish) (reason "done"))
EOF

"$SPG_BIN" agent --config "$T/run.spg" --fake-script "$T/script.txt" \
    --max-steps 4 --machine-history 4 >/dev/null || fail "agent run failed"
# tick 1 sees the explicit empty window; tick 2 sees tick 1's snapshot
strings "$T/j.sgj" | grep -q '(machine-history)' ||
    fail "the first tick's empty window is missing"
strings "$T/j.sgj" | grep -q '(machine-history (t 1 ' ||
    fail "the second tick never saw tick 1's snapshot"

# --- window 0 and flag-absent are byte-identical: the block does not exist --
rm -f "$T/j.sgj"
"$SPG_BIN" agent --config "$T/run.spg" --fake-script "$T/script.txt" \
    --max-steps 4 >/dev/null || fail "plain agent run failed"
if strings "$T/j.sgj" | grep -q 'machine-history'; then
    fail "history leaked into a run that never asked for it"
fi
rm -f "$T/j.sgj"
"$SPG_BIN" agent --config "$T/run.spg" --fake-script "$T/script.txt" \
    --max-steps 4 --machine-history 0 >/dev/null || fail "window-0 run failed"
if strings "$T/j.sgj" | grep -q 'machine-history'; then
    fail "window 0 must disable the block completely"
fi

# --- an out-of-range window is refused, not clamped silently ----------------
if "$SPG_BIN" agent --config "$T/run.spg" --fake-script "$T/script.txt" \
    --max-steps 2 --machine-history 999 >/dev/null 2>&1; then
    fail "an out-of-range window was accepted"
fi

# --- eval seeds the window from fixtures, and it reaches the PROMPT ---------
# The gate marker is the proof: the scripted fake's replies are rejected until
# the marker appears in the prompt, so the case can only pass if the seeded
# history text was actually rendered into the model input. 68000 mc exists
# nowhere but in the second seeded fixture.
cat > "$T/suite.spg" <<EOF
(eval_suite
 (config "$T/run.spg")
 (case (name "seeded") (script "$T/script.txt") (max_steps 4)
       (gate_marker "(temperature-mc 68000)")
       (machine_history "examples/eval/machine/states/trend_up_1.spg"
                        "examples/eval/machine/states/trend_up_2.spg")
       (machine "examples/eval/machine/states/trend_current.spg")
       (expect (termination finished))))
EOF
"$SPG_BIN" eval "$T/suite.spg" > "$T/eval.out" 2>&1 || {
    cat "$T/eval.out" >&2
    fail "seeded eval case failed"
}
grep -q '"name":"seeded","outcome":"pass"' "$T/eval.out" ||
    fail "the seeded window never reached the prompt"

# and the control: WITHOUT the seeding the same gate must stay shut
cat > "$T/control.spg" <<EOF
(eval_suite
 (config "$T/run.spg")
 (case (name "unseeded") (script "$T/script.txt") (max_steps 4)
       (gate_marker "(temperature-mc 68000)")
       (machine "examples/eval/machine/states/trend_current.spg")
       (expect (termination rejected))))
EOF
"$SPG_BIN" eval "$T/control.spg" > "$T/control.out" 2>&1 || {
    cat "$T/control.out" >&2
    fail "control case failed to run"
}
grep -q '"name":"unseeded","outcome":"pass","termination":"rejected"' \
    "$T/control.out" ||
    fail "the gate opened without the seeded history — the proof is void"

echo "test_cli_machine_history: PASS"
