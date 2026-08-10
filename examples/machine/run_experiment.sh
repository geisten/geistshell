#!/bin/sh
# Reproducible machine experiment (roadmap phase 8, #68).
#
# Starts a real workload, lets the agent observe and act on it, and writes one
# JSONL record describing what happened. Everything before this phase ran on
# fixtures; this is the first thing that measures the agent against a machine
# that is actually busy.
#
#   run_experiment.sh --scenario batch_pressure [--out results.jsonl]
#                     [--seconds N] [--mb N] [--model fake|geist] [--ci]
#
# Cleanup is the part that matters. A run that leaves a stopped workload behind
# has damaged the machine it was measuring, so every exit path — success,
# failure, interrupt — goes through the same trap.
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}
WORKLOAD_BIN=${WORKLOAD_BIN:-build/host-debug/bin/workload}

SCENARIO=batch_pressure
OUT=build/machine-experiments.jsonl
SECONDS_ARG=8
MB=64
MODEL=fake
CI_MODE=0
RUN_ID=""

while [ $# -gt 0 ]; do
    case "$1" in
        --scenario) SCENARIO=$2; shift 2 ;;
        --out) OUT=$2; shift 2 ;;
        --seconds) SECONDS_ARG=$2; shift 2 ;;
        --mb) MB=$2; shift 2 ;;
        --model) MODEL=$2; shift 2 ;;
        --run-id) RUN_ID=$2; shift 2 ;;
        --ci) CI_MODE=1; shift ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

# CI runs the same code paths at a size that fits inside a test suite. It is a
# smaller experiment, not a different one.
if [ "$CI_MODE" = "1" ]; then
    SECONDS_ARG=3
    MB=16
fi

WORKDIR=$(mktemp -d "${TMPDIR:-/tmp}/spg-experiment.XXXXXX")
JOURNAL="$WORKDIR/experiment.sgj"
BATCH_PID=""
CRITICAL_PID=""

# --- cleanup --------------------------------------------------------------
# Runs on success, on failure and on interrupt. Workloads are asked to stop and
# then made to; a workload that was paused by the agent is resumed first,
# because SIGTERM does not reach a stopped process.
cleanup() {
    status=$?
    for pid in $BATCH_PID $CRITICAL_PID; do
        [ -n "$pid" ] || continue
        kill -CONT "$pid" 2>/dev/null || true
        kill -TERM "$pid" 2>/dev/null || true
    done
    # Give them a moment to exit on their own before insisting.
    sleep 1
    for pid in $BATCH_PID $CRITICAL_PID; do
        [ -n "$pid" ] || continue
        kill -KILL "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    done
    rm -rf "$WORKDIR"
    exit $status
}
trap cleanup EXIT INT TERM

# The profile matches on the kernel's comm, which is the executable name. Two
# roles started from the same binary are indistinguishable — the first real Pi
# run labelled the CRITICAL process batch_job and paused it, because both were
# called "workload". A copy per role gives each its own name.
BATCH_BIN="$WORKDIR/batch-worker"
CRITICAL_BIN="$WORKDIR/critical-worker"
cp "$WORKLOAD_BIN" "$BATCH_BIN"
cp "$WORKLOAD_BIN" "$CRITICAL_BIN"

# --- the workloads --------------------------------------------------------
# A missing binary is not a scenario with no load — it is a broken experiment,
# and a record that looks like "the agent found nothing to do" would be a lie.
# The Pi found this the hard way: the build target was not part of the default
# build, so the first real run measured an idle machine and called it a result.
if [ "$SCENARIO" != "idle" ] && [ ! -x "$WORKLOAD_BIN" ]; then
    echo "run_experiment: no workload binary at $WORKLOAD_BIN" >&2
    exit 2
fi
case "$SCENARIO" in
    batch_pressure)
        "$CRITICAL_BIN" --mode critical --seconds "$SECONDS_ARG" >/dev/null &
        CRITICAL_PID=$!
        "$BATCH_BIN" --mode batch --seconds "$SECONDS_ARG" >/dev/null &
        BATCH_PID=$!
        ;;
    memory_pressure)
        "$BATCH_BIN" --mode memory --seconds "$SECONDS_ARG" --mb "$MB" \
            >/dev/null &
        BATCH_PID=$!
        ;;
    mixed)
        "$BATCH_BIN" --mode mixed --seconds "$SECONDS_ARG" --mb "$MB" \
            >/dev/null &
        BATCH_PID=$!
        "$CRITICAL_BIN" --mode critical --seconds "$SECONDS_ARG" >/dev/null &
        CRITICAL_PID=$!
        ;;
    idle)
        : ;;  # no load: the agent should conclude healthy and do nothing
    *)
        echo "unknown scenario: $SCENARIO" >&2; exit 2 ;;
esac

# Let the workloads reach a steady state before observing; sampling a process
# that started 3 ms ago measures process startup, not the scenario.
sleep 1

# --- profile and run config ----------------------------------------------
cat >"$WORKDIR/profile.spg" <<EOF
(process-profile
  (process "critical_app" (match "critical-worker") (role critical)
    (may_pause false) (may_stop false))
  (process "batch_job" (match "batch-worker") (role batch)
    (may_pause true) (may_stop true)))
EOF

cat >"$WORKDIR/run.spg" <<EOF
(run (model "fake.gguf") (policy "examples/machine-policy.spg")
 (scenario "examples/scenario.spg") (corpus "examples/corpus.spg")
 (journal "$JOURNAL") (seed 42)
 (budgets (inference_steps 6) (tokens 256) (shell_actions 0) (sim_actions 0)
  (memory_actions 0) (wall_ms 30000) (journal_bytes 1048576) (risk_bp 10000)
  (machine_actions 2)))
EOF

cat >"$WORKDIR/script.txt" <<'EOF'
(recommend (kind machine_pause_process) (capability "machine.process.pause") (target "batch_job") (cost 1) (uses_network false) (confidence_bp 9000) (reason "batch_pressure batch_job"))
(recommend (kind finish) (reason "healthy"))
EOF

START_MS=$(python3 -c 'import time; print(int(time.monotonic()*1000))')
AGENT_STATUS=0
AGENT_OUT="$WORKDIR/agent.out"
# The agent is allowed to fail: a failed run is a result, and the record has to
# say so rather than the experiment vanishing.
if [ "$MODEL" = "fake" ]; then
    "$SPG_BIN" agent --config "$WORKDIR/run.spg" \
        --fake-script "$WORKDIR/script.txt" --machine \
        --process-profile "$WORKDIR/profile.spg" --allow-exec \
        --machine-settle-ms 1500 --max-steps 4 >"$AGENT_OUT" 2>&1 || AGENT_STATUS=$?
else
    "$SPG_BIN" agent --config "$WORKDIR/run.spg" --machine \
        --process-profile "$WORKDIR/profile.spg" --allow-exec \
        --machine-settle-ms 1500 --constrained --max-steps 4 >"$AGENT_OUT" 2>&1 || AGENT_STATUS=$?
fi
END_MS=$(python3 -c 'import time; print(int(time.monotonic()*1000))')

# --- the record -----------------------------------------------------------
REPLAY="$WORKDIR/replay.jsonl"
"$SPG_BIN" replay "$JOURNAL" >"$REPLAY" 2>/dev/null || : >"$REPLAY"
STATES="$WORKDIR/states.txt"
strings "$JOURNAL" 2>/dev/null | grep -o '(machine-state.*' >"$STATES" || \
    : >"$STATES"

mkdir -p "$(dirname "$OUT")"
python3 - "$REPLAY" "$STATES" "$AGENT_OUT" "$OUT" <<PY
import json, sys, os, hashlib

replay_path, states_path, agent_out, out_path = sys.argv[1:5]
records = []
with open(replay_path, encoding="utf-8", errors="replace") as f:
    for line in f:
        line = line.strip()
        if line:
            try:
                records.append(json.loads(line))
            except json.JSONDecodeError:
                pass  # a run cut mid-write leaves a partial line; drop it

states = [s.strip() for s in open(states_path, encoding="utf-8",
                                  errors="replace").read().splitlines() if s]
text = open(agent_out, encoding="utf-8", errors="replace").read()

actions = [r for r in records if r.get("event") == "action"]
denials = [r for r in records if r.get("event") == "policy_decision"
           and r.get("decision") == "deny"]

# A run_id nobody chose is derived from the content, so two records can never
# silently collide the way a counter would.
run_id = "$RUN_ID" or hashlib.sha256(
    ("$SCENARIO" + str(len(records)) + str($START_MS)).encode()
).hexdigest()[:16]

# The goal: the critical workload survived and the batch load was reduced. Read
# from the observed states, not from what the model claimed.
def load_of(block):
    marker = "(cpu-load-bp "
    i = block.find(marker)
    if i < 0:
        return None
    value = block[i + len(marker):].split(")")[0].strip()
    return None if value == "unknown" else int(value)

initial = states[0] if states else ""
final = states[-1] if states else ""
def process_cpu(block, pid_name):
    marker = '(id "%s")' % pid_name
    i = block.find(marker)
    if i < 0:
        return None
    j = block.find("(cpu-bp ", i)
    if j < 0:
        return None
    value = block[j + len("(cpu-bp "):].split(")")[0].strip()
    return None if value == "unknown" else int(value)

# The goal, stated the way the scenario states it: the critical workload is
# still running and the unnecessary load is gone. Read from the observed state,
# never from what the model claimed.
#
# Not the system load: the FIRST sample of a run has no previous counters, so
# system utilisation is unknown by construction and comparing it would judge
# every experiment as failed. Per-process CPU in the final observation is what
# is actually measurable here.
critical_after = process_cpu(final, "critical_app")
batch_after = process_cpu(final, "batch_job")
goal_satisfied = bool(
    len(states) >= 2
    and batch_after == 0
    and critical_after is not None
    and critical_after > 0
)

# Did the machine the agent looked at actually contain the load we started?
# Without this an experiment can "succeed" against a machine that was idle.
workload_observed = "batch_job" in initial

record = {
    "run_id": run_id,
    "workload_observed": workload_observed,
    "scenario": "$SCENARIO",
    "model": "$MODEL",
    "ci_mode": bool($CI_MODE),
    "initial_state": initial,
    "final_state": final,
    "observations": len(states),
    "actions": [
        {"status": a.get("status"), "seq": a.get("seq")} for a in actions
    ],
    "policy_denials": len(denials),
    "steps": int(text.split("steps=")[1].split()[0]) if "steps=" in text
             else 0,
    "goal_satisfied": goal_satisfied,
    "critical_cpu_bp_final": critical_after,
    "batch_cpu_bp_final": batch_after,
    "agent_status": $AGENT_STATUS,
    "elapsed_ms": $END_MS - $START_MS,
    # Left null on purpose: no sensor here reports it, and inventing a number
    # would be worse than admitting the gap. Machine B (#75) can fill it.
    "energy_mj": None,
}
with open(out_path, "a", encoding="utf-8") as f:
    f.write(json.dumps(record) + "\n")
print(json.dumps(record))
PY
