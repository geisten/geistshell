#!/bin/sh
# Roadmap phase 8 (#68): the experiment runner.
#
# The point of this test is not that an experiment produces a nice number — on
# a machine without Linux telemetry it cannot. It is that the runner is safe to
# run: it always writes a record, and it never leaves a workload behind.
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}
WORKLOAD_BIN=${WORKLOAD_BIN:-build/host-debug/bin/workload}
RUNNER=examples/machine/run_experiment.sh
OUT=build/test-experiment.jsonl

export SPG_BIN WORKLOAD_BIN
rm -f "$OUT"

# --- 1. the workload is bounded by itself ---------------------------------
# No signal, no supervision: it must stop when its own deadline passes, or an
# experiment that crashes leaves a machine burning CPU forever.
#
# What is asserted is TERMINATION, not punctuality. The first version failed the
# suite by 76 seconds on a machine that was busy running the rest of the suite —
# a CPU workload asked for two seconds of CPU, and on a loaded host two seconds
# of CPU take much longer than two seconds of wall clock. That assertion
# measured the load on the test machine, which is exactly what
# .agent/AGENT.md's "prefer deterministic tests" rules out.
#
# The generous ceiling is a hang detector, not a stopwatch: it catches a
# workload that never returns at all, and says nothing about how fast it was.
START=$(date +%s)
"$WORKLOAD_BIN" --mode cpu --seconds 2 >/dev/null
ELAPSED=$(( $(date +%s) - START ))
if [ "$ELAPSED" -gt 120 ]; then
    echo "test_cli_experiment: FAIL — a 2s workload never returned (${ELAPSED}s)" >&2
    exit 1
fi

# Arguments cannot ask for more than the ceilings allow.
"$WORKLOAD_BIN" --mode memory --seconds 1 --mb 999999 >/dev/null

# --- 2. a smoke experiment always produces a record -----------------------
sh "$RUNNER" --ci --scenario batch_pressure --out "$OUT" >/dev/null
sh "$RUNNER" --ci --scenario idle --out "$OUT" >/dev/null

python3 - "$OUT" <<'PY'
import json, sys

rows = [json.loads(l) for l in open(sys.argv[1])]
assert len(rows) == 2, rows

required = {"run_id", "scenario", "model", "initial_state", "final_state",
            "actions", "policy_denials", "steps", "goal_satisfied",
            "elapsed_ms", "energy_mj", "agent_status", "workload_observed"}
for r in rows:
    missing = required - set(r)
    assert not missing, f"record is missing {missing}"
    # Absent measurements stay null. A zero here would read as "measured, and
    # it was zero" — the mistake the whole telemetry layer is built to avoid.
    assert r["energy_mj"] is None, r

# Two runs of the same scenario must not share an id, or results silently
# overwrite each other in analysis.
assert rows[0]["run_id"] != rows[1]["run_id"], rows

# The file stays valid JSONL: every line parses on its own.
for line in open(sys.argv[1]):
    json.loads(line)
PY

# A runner that cannot start its load must fail loudly rather than report an
# idle machine as a finding.
if SPG_BIN=$SPG_BIN WORKLOAD_BIN=/nonexistent/workload sh "$RUNNER" --ci \
        --out /dev/null >/dev/null 2>&1; then
    echo "test_cli_experiment: FAIL — ran an experiment with no workload" >&2
    exit 1
fi

# --- 3. nothing is left running -------------------------------------------
# The runner's own workloads are gone, including any the agent had paused —
# SIGTERM does not reach a stopped process, so cleanup has to resume first.
if pgrep -f "$WORKLOAD_BIN" >/dev/null 2>&1; then
    echo "test_cli_experiment: FAIL — a workload survived the runner" >&2
    pkill -f "$WORKLOAD_BIN" 2>/dev/null || true
    exit 1
fi

# --- 4. an interrupted runner still cleans up ------------------------------
# The case that matters in practice: somebody presses ctrl-c mid-experiment.
sh "$RUNNER" --scenario batch_pressure --seconds 30 --out /dev/null \
    >/dev/null 2>&1 &
RUNNER_PID=$!
sleep 3
kill -INT "$RUNNER_PID" 2>/dev/null || true
wait "$RUNNER_PID" 2>/dev/null || true
sleep 2

if pgrep -f "$WORKLOAD_BIN" >/dev/null 2>&1; then
    echo "test_cli_experiment: FAIL — interrupt left orphaned workloads" >&2
    pkill -f "$WORKLOAD_BIN" 2>/dev/null || true
    exit 1
fi

echo "test_cli_experiment: PASS"
