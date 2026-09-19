#!/bin/sh
# End-to-end against a simulated machine: geistshell drives a plant through
# exec channels and the plant's temperature responds. The point is that
# nothing here is a mock — the same exec contract, the same refusals that any
# real channel program would meet: a sensor is a program that prints a number,
# an actuator is a program that takes one.
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}
PLANT=examples/machine/plant/heater.py
DIR=build/test-cli-device-plant
STATE=$DIR/state.json

if ! command -v python3 >/dev/null 2>&1; then
    echo "test_cli_device: SKIP (no python3)"
    exit 0
fi

rm -rf "$DIR"
mkdir -p "$DIR"

# speed 20 puts the thermal time constant at about 125 ms, so every settle
# below is complete long before it is read. Direction and latch are asserted,
# never a wall-clock duration.
python3 "$PLANT" "$STATE" init 20

# One executable per channel — the exec contract in its plainest form. The
# value arrives as "$1", never inside a shell line an interpreter re-reads.
for CHANNEL in temp heater tripped reset; do
    {
        echo "#!/bin/sh"
        echo "exec python3 $PWD/$PLANT $PWD/$STATE $CHANNEL \"\$@\""
    } >"$DIR/$CHANNEL"
    chmod 0755 "$DIR/$CHANNEL"
done

# The heater's safe value is 0: on loss of contact the vessel stops being
# heated. Declaring (safe ...) is what MAKES it writable, so the config cannot
# omit the decision.
cat >"$DIR/plant.spg" <<EOF
(device
  (channel (name "temp")    (program "$PWD/$DIR/temp")    (range -400 9000))
  (channel (name "heater")  (program "$PWD/$DIR/heater")  (range 0 100) (safe 0))
  (channel (name "tripped") (program "$PWD/$DIR/tripped") (range 0 1))
  (channel (name "reset")   (program "$PWD/$DIR/reset")   (range 0 1) (safe 0)))
EOF

dev() {
    "$SPG_BIN" device --config "$DIR/plant.spg" "$@"
}

value_of() {
    sed -n 's/.*(value \(-\{0,1\}[0-9][0-9]*\)).*/\1/p'
}

fail() {
    echo "test_cli_device: FAIL — $1" >&2
    exit 1
}

# Wait for a CONDITION, not for a number of tries. The plant advances with wall
# time, so a fixed count is a bet on how fast the machine is: on a loaded
# 4-vCPU runner the old `while [ $i -lt 50 ]` expired before the vessel had
# settled, and the failure surfaced two blocks later as a missing journal line.
# Budget is whole seconds, generous on purpose; the message names what was missed.
wait_for() { # <seconds> <what-was-missed> <predicate...>
    _deadline=$(($(date +%s) + $1))
    _what=$2
    shift 2
    while ! "$@"; do
        [ "$(date +%s)" -lt "$_deadline" ] || fail "$_what"
        sleep 0.1
    done
}

tripped_is_latched() { [ "$(dev read tripped | value_of)" = "1" ]; }
temp_is_ambient()    { [ "$(dev read temp | value_of)" = "200" ]; }
heater_is_off()      { [ "$(dev read heater | value_of)" = "0" ]; }

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
wait_for 30 "100% heat never reached the over-temperature trip" tripped_is_latched

# The device refuses while latched, and geistshell reports that refusal rather
# than treating an unacknowledged command as done.
if dev write heater 50 >/dev/null 2>&1; then
    fail "machine accepted a heater command while tripped"
fi

dev write reset 1 >/dev/null || fail "reset rejected"
[ "$(dev read tripped | value_of)" = "0" ] || fail "reset did not clear the trip"

# Let the vessel cool back to ambient so the agent below sees a cold plant. The
# trip already zeroed the power, but say it out loud: a reset clears the latch
# only, and a wait whose target nothing drives towards is a trap.
dev write heater 0 >/dev/null || fail "resetting heater after the trip failed"
wait_for 30 "vessel never cooled back to ambient (200)" temp_is_ambient

echo "test_cli_device: PASS"

# --- the agent action, end to end -------------------------------------------
# The point of this block is that nothing between the model output and the
# plant is stubbed: a recommendation goes through the policy gate, the device
# executor and the channel program, and the simulated vessel gets warmer.
FAKE=build/test-cli-device-fake.txt
printf '(recommend (kind device_write) (capability "device") (cost 1) (uses_network false) (confidence_bp 9000) (target "heater") (value 40) (reason "warm the vessel"))\n(recommend (kind finish) (reason "done"))\n' >"$FAKE"

cat >"$DIR/agent.spg" <<EOF
(device
  (channel (name "heater") (program "$PWD/$DIR/heater") (range 0 100) (safe 0))
  (channel (name "temp")   (program "$PWD/$DIR/temp")   (range -400 9000)))
EOF

# Without the capability the write must be denied — default deny is the whole
# reason an irreversible action is tolerable at all.
"$SPG_BIN" agent --config examples/run.spg --fake-script "$FAKE" --max-steps 2 \
    --allow-exec --device "$DIR/agent.spg" \
    >/dev/null 2>&1 || true
[ "$(dev read heater | value_of)" = "0" ] ||
    fail "a policy without the device capability still moved the machine"

# With it, the plant actually responds. The journal is truncated first: every
# agent run below appends to the same file, so an unscoped `grep` could be
# satisfied by a LATER block — the checks then pass without ever looking at the
# run they describe. #118's watchdog block and the #119 network block both write
# a (device-state ...) of their own.
rm -f build/device-demo.sgj
wait_for 30 "vessel never cooled back to ambient before the agent run" temp_is_ambient
heater_is_off || fail "heater was not off before the agent run"

"$SPG_BIN" agent --config examples/device/run.spg --fake-script "$FAKE" \
    --max-steps 2 --allow-exec --device "$DIR/agent.spg" \
    >/dev/null || fail "agent run failed"

[ "$(dev read heater | value_of)" = "40" ] ||
    fail "the agent's device_write did not reach the machine"
strings build/device-demo.sgj | grep -q '(outcome written)' ||
    fail "the write is missing from the journal"

# The readings the decision was made ON, not just the action it produced. The
# context is journaled whole as MODEL_INPUT, so a (device-state ...) block in
# the prompt IS the audit record — a replay that shows what the agent did but
# not what it saw is half a record.
# Any reading, not the exact 200: the claim is that the numbers the decision was
# made ON are in the record. Pinning the temperature turns that claim into a bet
# on how much plant time passed before the sample.
strings build/device-demo.sgj | grep -Eq '\(device-state \(heater 0\) \(temp -?[0-9]+\)\)' ||
    fail "the plant readings never reached the journaled context"

# And the loop actually closes: the context AFTER the write shows the plant the
# write left behind, not the one the decision was made on. Without the
# post-action re-sample both blocks would read (heater 0) and the agent would
# steer for the rest of the run on a snapshot it had already invalidated.
strings build/device-demo.sgj | grep -q '(device-state (heater 40)' ||
    fail "the context was not re-sampled after the write"

echo "test_cli_device: PASS (agent action)"

# --- #118: the watchdog fires without a device_write to the dead channel ----
# A dead actuator (its program always fails) while OTHER channels keep
# answering: before #118 the shared contact flag let any live channel mask the
# loss, and the check only ran when a device_write reached the executor. Now
# the deadline is per writable channel and serviced every tick, so the run
# journals the expiry and drives the safe state even though the model never
# addressed the dead channel again.
cat >"$DIR/deadheater" <<'EOS'
#!/bin/sh
exit 1
EOS
chmod 0755 "$DIR/deadheater"

cat >"$DIR/watchdog.spg" <<EOF
(device
  (channel (name "heater") (program "$PWD/$DIR/deadheater") (range 0 100) (safe 0))
  (channel (name "reset")  (program "$PWD/$DIR/reset")      (range 0 1) (safe 0))
  (channel (name "temp")   (program "$PWD/$DIR/temp")       (range -400 9000)))
EOF

WFAKE=build/test-cli-device-watchdog-fake.txt
printf '%s\n%s\n%s\n%s\n' \
    '(recommend (kind device_write) (capability "device") (cost 1) (uses_network false) (confidence_bp 9000) (target "reset") (value 0) (reason "tick 1"))' \
    '(recommend (kind device_write) (capability "device") (cost 1) (uses_network false) (confidence_bp 9000) (target "reset") (value 0) (reason "tick 2"))' \
    '(recommend (kind device_write) (capability "device") (cost 1) (uses_network false) (confidence_bp 9000) (target "reset") (value 0) (reason "tick 3"))' \
    '(recommend (kind finish) (reason "done"))' >"$WFAKE"

"$SPG_BIN" agent --config examples/device/run.spg --fake-script "$WFAKE" \
    --max-steps 6 --allow-exec --device "$DIR/watchdog.spg" \
    --device-watchdog-steps 1 >/dev/null || fail "watchdog agent run failed"

strings build/device-demo.sgj |
    grep -q '(device_watchdog (outcome expired) (safe_state failed))' ||
    fail "a dead actuator behind live channels never tripped the tick-level watchdog"

echo "test_cli_device: PASS (tick-level watchdog)"

# --- #119: a network channel is refused from TRUSTED config -----------------
# The operator declares (network true) on the channel; the plant policy says
# (network_default deny). The model's form always carries uses_network false —
# the gate must derive the truth from the loaded table and refuse BEFORE any
# fork, while a local channel (the block above) keeps working.
cat >"$DIR/network.spg" <<EOF
(device
  (channel (name "heater") (program "$PWD/$DIR/heater") (range 0 100) (safe 0)
           (network true))
  (channel (name "temp")   (program "$PWD/$DIR/temp")   (range -400 9000)))
EOF

# Reset the plant so an (incorrectly) executed write would be visible.
dev write heater 0 >/dev/null || fail "resetting heater failed"

"$SPG_BIN" agent --config examples/device/run.spg --fake-script "$FAKE" \
    --max-steps 2 --allow-exec --device "$DIR/network.spg" \
    >/dev/null 2>&1 || true

[ "$(dev read heater | value_of)" = "0" ] ||
    fail "a network channel under network_default deny still moved the machine"
strings build/device-demo.sgj | grep -q 'SPG_POLICY_DENY_NETWORK' ||
    fail "the network denial is missing from the journal"

echo "test_cli_device: PASS (network channel refused from config)"
