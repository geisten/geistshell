#!/bin/sh
# End-to-end against a simulated machine: geistshell drives a Modbus TCP plant
# and the plant's temperature responds. The point is that nothing here is a
# mock — the same client code, the same wire format, the same refusals that a
# real Modbus device would meet.
#
# Port 0 lets the kernel pick, so a developer already running something on 5502
# does not get a mysterious failure.
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}
PLANT=examples/machine/plant/heater.py
LOG=build/test-cli-device-plant.log

if ! command -v python3 >/dev/null 2>&1; then
    echo "test_cli_device: SKIP (no python3)"
    exit 0
fi

# speed 20 puts the thermal time constant at about 125 ms, so every settle
# below is complete long before it is read. The tick rate is fixed at 20 Hz
# whatever the speed, so this fixture does not disturb the load-sensitive
# machine tests that run after it. Direction and latch are asserted, never
# a wall-clock duration.
python3 "$PLANT" --port 0 --speed 20 >"$LOG" 2>&1 &
PLANT_PID=$!
trap 'kill "$PLANT_PID" 2>/dev/null || true' EXIT

PORT=""
i=0
while [ $i -lt 50 ]; do
    PORT=$(sed -n 's/.*:\([0-9][0-9]*\)$/\1/p' "$LOG" 2>/dev/null | head -1)
    [ -n "$PORT" ] && break
    i=$((i + 1))
    sleep 0.1
done
if [ -z "$PORT" ]; then
    echo "test_cli_device: FAIL — plant did not start" >&2
    cat "$LOG" >&2
    exit 1
fi

# The heater's safe value is 0: on loss of contact the vessel stops being
# heated. Declaring it is mandatory, so this string cannot omit the decision.
CH="--channel temp:0:-400:9000:r --channel heater:1:0:100:w:0"
CH="$CH --channel tripped:3:0:1:r --channel reset:4:0:1:w:0"

dev() {
    # shellcheck disable=SC2086
    "$SPG_BIN" device --port "$PORT" $CH "$@"
}

value_of() {
    sed -n 's/.*(value \(-\{0,1\}[0-9][0-9]*\)).*/\1/p'
}

fail() {
    echo "test_cli_device: FAIL — $1" >&2
    exit 1
}

# --- reading a machine ------------------------------------------------------
COLD=$(dev read temp | value_of)
[ "$COLD" = "200" ] || fail "cold plant read $COLD, expected 200 (20.0 C)"

# --- writing moves it -------------------------------------------------------
dev write heater 30 >/dev/null || fail "write heater 30 rejected"
sleep 1
WARM=$(dev read temp | value_of)
[ "$WARM" -gt "$COLD" ] || fail "heater on, temperature did not rise ($WARM)"
[ "$WARM" -lt 900 ] || fail "30% should settle well below the trip ($WARM)"

# --- the range is a refusal, not a clamp ------------------------------------
if dev write heater 150 >/dev/null 2>&1; then
    fail "out-of-range write was accepted"
fi
AFTER=$(dev read heater | value_of)
[ "$AFTER" = "30" ] || fail "refused write still reached the machine ($AFTER)"

# --- a read-only channel is refused -----------------------------------------
if dev write temp 500 >/dev/null 2>&1; then
    fail "write to a read-only channel was accepted"
fi

# --- an unknown channel is refused ------------------------------------------
if dev read nosuch >/dev/null 2>&1; then
    fail "unknown channel was accepted"
fi

# --- the machine's own interlock --------------------------------------------
dev write heater 100 >/dev/null || fail "write heater 100 rejected"
i=0
TRIPPED=0
while [ $i -lt 50 ]; do
    TRIPPED=$(dev read tripped | value_of)
    [ "$TRIPPED" = "1" ] && break
    i=$((i + 1))
    sleep 0.1
done
[ "$TRIPPED" = "1" ] || fail "100% heat never reached the over-temperature trip"

# The device refuses while latched, and geistshell reports that refusal rather
# than treating an unacknowledged command as done.
if dev write heater 50 >/dev/null 2>&1; then
    fail "machine accepted a heater command while tripped"
fi

dev write reset 1 >/dev/null || fail "reset rejected"
[ "$(dev read tripped | value_of)" = "0" ] || fail "reset did not clear the trip"

echo "test_cli_device: PASS"

# --- the agent action, end to end -------------------------------------------
# The point of this block is that nothing between the model output and the
# plant is stubbed: a recommendation goes through the policy gate, the device
# executor and the Modbus client, and the simulated vessel gets warmer.
FAKE=build/test-cli-device-fake.txt
printf '(recommend (kind device_write) (capability "device") (cost 1) (uses_network false) (confidence_bp 9000) (target "heater") (value 40) (reason "warm the vessel"))\n(recommend (kind finish) (reason "done"))\n' >"$FAKE"

AGENT_CH="--device-channel heater:1:0:100:w:0 --device-channel temp:0:-400:9000:r"

# Without the capability the write must be denied — default deny is the whole
# reason an irreversible action is tolerable at all.
# shellcheck disable=SC2086
"$SPG_BIN" agent --config examples/run.spg --fake-script "$FAKE" --max-steps 2 \
    --allow-exec --device-host 127.0.0.1 --device-port "$PORT" $AGENT_CH \
    >/dev/null 2>&1 || true
[ "$(dev read heater | value_of)" = "0" ] ||
    fail "a policy without the device capability still moved the machine"

# With it, the plant actually responds.
# shellcheck disable=SC2086
"$SPG_BIN" agent --config examples/device/run.spg --fake-script "$FAKE" \
    --max-steps 2 --allow-exec --device-host 127.0.0.1 --device-port "$PORT" \
    $AGENT_CH >/dev/null || fail "agent run failed"

[ "$(dev read heater | value_of)" = "40" ] ||
    fail "the agent's device_write did not reach the machine"
strings build/device-demo.sgj | grep -q '(outcome written)' ||
    fail "the write is missing from the journal"

# The readings the decision was made ON, not just the action it produced. The
# context is journaled whole as MODEL_INPUT, so a (device-state ...) block in
# the prompt IS the audit record — a replay that shows what the agent did but
# not what it saw is half a record.
strings build/device-demo.sgj | grep -q '(device-state (heater 0) (temp 200))' ||
    fail "the plant readings never reached the journaled context"

# And the loop actually closes: the context AFTER the write shows the plant the
# write left behind, not the one the decision was made on. Without the
# post-action re-sample both blocks would read (heater 0) and the agent would
# steer for the rest of the run on a snapshot it had already invalidated.
strings build/device-demo.sgj | grep -q '(device-state (heater 40) (temp 200))' ||
    fail "the context was not re-sampled after the write"

echo "test_cli_device: PASS (agent action)"
