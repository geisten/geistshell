#!/bin/sh
# Roadmap phase 6 (#66): a machine action end to end — gate, executor, journal,
# replay — driven by the CLI rather than by a unit test's hand-built structs.
#
# On Linux it pauses a real `sleep` and checks the process actually stopped. On
# every other platform the snapshot has no processes, so the SAME run must be
# denied rather than quietly do nothing, and that denial is what gets checked.
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}
JOURNAL=build/machine-demo.sgj
PROFILE=build/machine-profile.spg
SCRIPT=build/machine-fake.txt

mkdir -p build
cat >"$PROFILE" <<'EOF'
(process-profile
  (process "batch_job" (match "sleep") (role batch)
    (may_pause true) (may_stop true))
  (process "critical_app" (match "init") (role critical)
    (may_pause false) (may_stop false)))
EOF

pause_script() {
    printf '(recommend (kind machine_pause_process) (capability "machine.process.pause") (target "%s") (cost 1) (uses_network false) (confidence_bp 9000) (reason "reduce load"))\n(recommend (kind finish) (reason "done"))\n' "$1" >"$SCRIPT"
}

run_agent() {
    rm -f "$JOURNAL"
    "$SPG_BIN" agent --config examples/machine-run.spg --fake-script "$SCRIPT" \
        --machine --process-profile "$PROFILE" --allow-exec --max-steps 3
}

CHILD=""
cleanup() {
    if [ -n "$CHILD" ]; then
        kill -CONT "$CHILD" 2>/dev/null || true
        kill -KILL "$CHILD" 2>/dev/null || true
    fi
}
trap cleanup EXIT

if [ "$(uname -s)" = "Linux" ]; then
    sleep 120 &
    CHILD=$!
    sleep 0.2

    pause_script batch_job
    OUT=$(run_agent)
    case "$OUT" in
    *termination=finished*) ;;
    *)
        echo "test_cli_machine_run: FAIL — expected the pause to be allowed" >&2
        echo "  $OUT" >&2
        exit 1
        ;;
    esac
    if ! strings "$JOURNAL" | grep -q "(outcome ok)"; then
        echo "test_cli_machine_run: FAIL — no successful machine_action" >&2
        exit 1
    fi
    # Since phase 6b the run releases what it paused before it exits, so the
    # child is running again by the time we can look. The evidence that the
    # pause happened is the journal; the evidence that it was undone is the
    # child's state plus the released= line.
    case "$OUT" in
    *released=1*) ;;
    *)
        echo "test_cli_machine_run: FAIL — the pause was not released" >&2
        echo "  $OUT" >&2
        exit 1
        ;;
    esac
    STATE=$(awk '{print $3}' "/proc/$CHILD/stat" 2>/dev/null || echo "?")
    if [ "$STATE" = "T" ]; then
        echo "test_cli_machine_run: FAIL — child left stopped after the run" >&2
        exit 1
    fi

    # A protected process must be refused by the policy layer, and the journal
    # must say so — "denied" alone would not distinguish it from running out of
    # budget.
    pause_script critical_app
    OUT=$(run_agent || true)
    if ! strings "$JOURNAL" | grep -q "SPG_POLICY_DENY_PROCESS_PROTECTED"; then
        echo "test_cli_machine_run: FAIL — critical process was not protected" >&2
        exit 1
    fi
else
    # No snapshot means no observed process, and an action on something we did
    # not observe is a guess. Denial, not a silent no-op.
    pause_script batch_job
    OUT=$(run_agent || true)
    if ! strings "$JOURNAL" | grep -q "SPG_POLICY_DENY_PROCESS_IDENTITY"; then
        echo "test_cli_machine_run: FAIL — expected an identity denial" >&2
        exit 1
    fi
fi

# Whatever happened, it must replay: a decision that cannot be reproduced is a
# decision nobody can audit.
"$SPG_BIN" replay "$JOURNAL" >build/machine-replay.jsonl
if [ ! -s build/machine-replay.jsonl ]; then
    echo "test_cli_machine_run: FAIL — replay produced nothing" >&2
    exit 1
fi

echo "test_cli_machine_run: PASS"
