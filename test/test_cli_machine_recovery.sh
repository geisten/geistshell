#!/bin/sh
# Roadmap phase 6b (#80): nothing stays paused.
#
# Two defences, and this checks both. The first is the release path: whatever
# ends the run, the ledger is emptied and everything we stopped is restarted.
# The second is recovery: SIGKILL is not catchable, so a pause can outlive the
# process that owed the resume — the next start reads the journal and finishes
# the job.
#
# Linux only; elsewhere the executor never signals in the first place.
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}
JOURNAL=build/machine-recovery.sgj
PROFILE=build/machine-recovery-profile.spg
SCRIPT=build/machine-recovery-fake.txt
RUNCFG=build/machine-recovery-run.spg

if [ "$(uname -s)" != "Linux" ]; then
    echo "test_cli_machine_recovery: SKIP (no process signals here)"
    exit 0
fi

mkdir -p build
cat >"$PROFILE" <<'EOF'
(process-profile
  (process "batch_job" (match "sleep") (role batch)
    (may_pause true) (may_stop true)))
EOF
sed "s|build/machine-demo.sgj|$JOURNAL|" examples/machine-run.spg >"$RUNCFG"

CHILD=""
cleanup() {
    if [ -n "$CHILD" ]; then
        kill -CONT "$CHILD" 2>/dev/null || true
        kill -KILL "$CHILD" 2>/dev/null || true
    fi
}
trap cleanup EXIT

state_of() { awk '{print $3}' "/proc/$1/stat" 2>/dev/null || echo "?"; }

# --- 1. the release path: pause, then end the run normally ----------------
sleep 120 &
CHILD=$!
sleep 0.2

# The run pauses and then finishes. Without the release the child would still
# be stopped when the agent exits.
printf '(recommend (kind machine_pause_process) (capability "machine.process.pause") (target "batch_job") (cost 1) (uses_network false) (confidence_bp 9000) (reason "reduce load"))\n(recommend (kind finish) (reason "done"))\n' >"$SCRIPT"
rm -f "$JOURNAL"
OUT=$("$SPG_BIN" agent --config "$RUNCFG" --fake-script "$SCRIPT" \
    --process-profile "$PROFILE" --allow-exec --max-steps 3)

case "$OUT" in
*released=1*) ;;
*)
    echo "test_cli_machine_recovery: FAIL — the run did not release its pause" >&2
    echo "  $OUT" >&2
    exit 1
    ;;
esac
if [ "$(state_of "$CHILD")" = "T" ]; then
    echo "test_cli_machine_recovery: FAIL — child still stopped after the run" >&2
    exit 1
fi
kill -KILL "$CHILD"
wait "$CHILD" 2>/dev/null || true

# --- 2. one machine run per journal ---------------------------------------
# Two runs sharing a journal would interleave records into one hash chain and
# each other's recovery would resume the other's pauses. The second run is told
# to use its own journal rather than made to wait.
sleep 120 &
CHILD=$!
sleep 0.2
printf '(recommend (kind machine_pause_process) (capability "machine.process.pause") (target "batch_job") (cost 1) (uses_network false) (confidence_bp 9000) (reason "hold"))\n(recommend (kind finish) (reason "done"))\n' >"$SCRIPT"

# Hold the lock from the outside, the way a concurrent run would.
exec 9>"$JOURNAL.lock"
if command -v flock >/dev/null 2>&1; then
    flock -n 9 || {
        echo "test_cli_machine_recovery: FAIL — could not take the lock" >&2
        exit 1
    }
    if "$SPG_BIN" agent --config "$RUNCFG" --fake-script "$SCRIPT" \
        --process-profile "$PROFILE" --allow-exec --max-steps 3 2>/dev/null; then
        echo "test_cli_machine_recovery: FAIL — a second run started anyway" >&2
        exit 1
    fi
    exec 9>&-
fi

# The remaining defences — recovery after an uncatchable kill, and the guardian
# that covers the window before the next start — are checked in
# machine_action_probe.c, where the crash can be simulated without a race.

echo "test_cli_machine_recovery: PASS"
