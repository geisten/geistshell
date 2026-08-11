#!/bin/sh
# geistshell drives a Gymnasium environment over Modbus.
#
# Foreign physics, foreign difficulty. heater.py is a machine I wrote, so a
# controller written against it is graded by its own examiner; Pendulum-v1 is
# not. And geistshell needed no change for it — same client, same channel
# table, different machine.
#
# Skipped rather than failed when the venv is absent: `make test` on a fresh
# checkout must not require a pip install.
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}
PY=build/gymenv/bin/python
BRIDGE=examples/machine/plant/gym_bridge.py
LOG=build/test-cli-gym.log

if [ ! -x "$PY" ] || ! "$PY" -c "import gymnasium" 2>/dev/null; then
    echo "test_cli_gym: SKIP (no build/gymenv; see docs/machine-intelligence/Devices.md)"
    exit 0
fi

"$PY" "$BRIDGE" --env Pendulum-v1 --port 0 --seed 0 >"$LOG" 2>&1 &
BRIDGE_PID=$!
trap 'kill "$BRIDGE_PID" 2>/dev/null || true' EXIT

PORT=""
i=0
while [ $i -lt 50 ]; do
    PORT=$(sed -n 's/.*:\([0-9][0-9]*\)$/\1/p' "$LOG" 2>/dev/null | head -1)
    [ -n "$PORT" ] && break
    i=$((i + 1))
    sleep 0.2
done
[ -n "$PORT" ] || { echo "test_cli_gym: FAIL — bridge did not start" >&2; cat "$LOG" >&2; exit 1; }

CH="--channel vel:2:-32768:32767:r --channel steps:103:0:32767:r"
CH="$CH --channel torque:200:-200:200:w:0 --channel commit:250:1:1:w:1"
CH="$CH --channel reset:251:1:1:w:1"

dev() {
    # shellcheck disable=SC2086
    "$SPG_BIN" device --port "$PORT" $CH "$@"
}
value_of() { sed -n 's/.*(value \(-\{0,1\}[0-9][0-9]*\)).*/\1/p'; }
fail() { echo "test_cli_gym: FAIL — $1" >&2; exit 1; }

# --- an RL environment advances only when acted upon -------------------------
# Reading does not step it. That is the property that makes this a good fit for
# an agent whose own clock is a step counter rather than a wall clock.
[ "$(dev read steps | value_of)" = "0" ] || fail "fresh episode is not at step 0"
dev read vel >/dev/null
[ "$(dev read steps | value_of)" = "0" ] || fail "a read advanced the environment"

# --- staging is not acting ---------------------------------------------------
dev write torque 200 >/dev/null || fail "staging the action was refused"
[ "$(dev read steps | value_of)" = "0" ] ||
    fail "writing the action register stepped the environment"

BEFORE=$(dev read vel | value_of)
dev write commit 1 >/dev/null || fail "commit was refused"
[ "$(dev read steps | value_of)" = "1" ] || fail "commit did not step"
AFTER=$(dev read vel | value_of)
[ "$BEFORE" != "$AFTER" ] || fail "full torque left the pendulum unmoved"

# --- the channel table bounds a foreign machine too --------------------------
if dev write torque 500 >/dev/null 2>&1; then
    fail "an action outside the environment's range was accepted"
fi

# --- seeded, so a replayed run meets the same episode ------------------------
dev write reset 1 >/dev/null || fail "reset was refused"
[ "$(dev read steps | value_of)" = "0" ] || fail "reset did not restart the episode"
[ "$(dev read vel | value_of)" = "$(dev read vel | value_of)" ] ||
    fail "two reads of a stopped environment disagreed"

echo "test_cli_gym: PASS"
