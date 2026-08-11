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

CH="--channel temp:0:-400:9000:r --channel heater:1:0:100:w"
CH="$CH --channel tripped:3:0:1:r --channel reset:4:0:1:w"

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
