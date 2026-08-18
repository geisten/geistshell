#!/bin/sh
# Smoke test (NOT run by `make test` — needs the GGUF, is slow and
# non-deterministic): exercises the real-model eval path end to end. A
# `(model "geist")` case loads the actual engine, runs the governed loop against
# the scenario, and is scored like any other case. The small local model is
# unreliable at emitting the recommendation grammar, so this asserts the PATH
# works (model loads, runs, is judged, JSONL emitted) — not that it succeeds.
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}
MODEL=${GEISTSHELL_MODEL:-/Users/germar/workspace/geist/gguf_artifacts/gemma4-e2b-Q4_K_M.gguf}

if [ ! -f "$MODEL" ]; then
    echo "smoke_eval_real: SKIP (model not found: $MODEL)"
    exit 0
fi

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

cat > "$T/run.spg" <<EOF
(run
 (model "$MODEL")
 (policy "examples/policy.spg")
 (scenario "examples/scenario.spg")
 (corpus "examples/corpus.spg")
 (journal "$T/j.sgj")
 (seed 42)
 (budgets (inference_steps 3) (tokens 512) (shell_actions 1) (sim_actions 8) (memory_actions 8) (wall_ms 120000)))
EOF
cat > "$T/suite.spg" <<EOF
(eval_suite
 (config "$T/run.spg")
 (case (name "real-sim") (model "geist") (max_steps 3)
       (expect (termination finished))))
EOF

# macOS has no timeout(1); cap the run with perl's alarm.
OUT=$(perl -e 'alarm 240; exec @ARGV' "$SPG_BIN" eval "$T/suite.spg" 2>&1) || true
printf '%s\n' "$OUT"

# the real-model path produced a structured verdict for the case + a summary
printf '%s\n' "$OUT" | grep -q '"name":"real-sim","outcome":'
printf '%s\n' "$OUT" | grep -q '"suite":'

echo "smoke_eval_real: PASS"
