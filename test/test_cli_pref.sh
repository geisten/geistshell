#!/bin/sh
# #28: user-profile memory, end to end. Write-on-evidence at the CLI, the
# budgeted profile line injected into a later run, an off-switch, and the
# capability-invariance boundary — a preference shapes framing, never what
# the policy gate permits.
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

fail() {
    echo "test_cli_pref: FAIL — $1" >&2
    exit 1
}

MEM=$T/mem

# --- write-on-evidence: a model self-assertion is refused -------------------
"$SPG_BIN" memory pref editor vim --evidence asserted --dir "$MEM" \
    > "$T/a.out" 2>&1 && fail "an asserted preference was written"
grep -q '"wrote":false' "$T/a.out" || fail "asserted pref did not report wrote:false"
test ! -f "$MEM/pref-editor.md" || fail "asserted pref left a file"

# a repeated choice below threshold is still refused
"$SPG_BIN" memory pref editor vim --evidence repeated --count 1 --dir "$MEM" \
    > /dev/null 2>&1 && fail "a single observation was written"
test ! -f "$MEM/pref-editor.md" || fail "count-1 repeated left a file"

# --- at the threshold it writes ---------------------------------------------
"$SPG_BIN" memory pref editor vim --evidence repeated --count 2 --dir "$MEM" \
    > "$T/w.out" 2>&1 || fail "a repeated (count 2) preference was refused"
grep -q '"wrote":true' "$T/w.out" || fail "count-2 did not write"
test -f "$MEM/pref-editor.md" || fail "the pref file is missing"
# a correction is authoritative on the first signal
"$SPG_BIN" memory pref units metric --evidence correction --count 1 \
    --dir "$MEM" > /dev/null 2>&1 || fail "a correction was refused"

# --- the profile renders as ONE budgeted line -------------------------------
"$SPG_BIN" memory profile --dir "$MEM" > "$T/p.out" || fail "profile render failed"
grep -q '^(profile "editor=vim; units=metric")$' "$T/p.out" ||
    fail "unexpected profile line: $(cat "$T/p.out")"
[ "$(wc -l < "$T/p.out")" -eq 1 ] || fail "the profile is more than one line"

# --- the profile is injected into a later run's context ---------------------
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
printf '(recommend (kind simulator) (capability "sim.act") (cost 1) (uses_network false) (confidence_bp 7000) (reason "x"))\n(recommend (kind finish) (reason "done"))\n' > "$T/s.txt"

"$SPG_BIN" agent --config "$T/run.spg" --fake-script "$T/s.txt" \
    --memory-dir "$MEM" --max-steps 4 > /dev/null || fail "run failed"
strings "$T/j.sgj" | grep -q '(profile "editor=vim; units=metric")' ||
    fail "the profile was not injected into the run context"

# --- the off-switch: byte-identical to a run without any profile ------------
rm -f "$T/j.sgj"
"$SPG_BIN" agent --config "$T/run.spg" --fake-script "$T/s.txt" \
    --memory-dir "$MEM" --max-steps 4 --no-profile > /dev/null ||
    fail "off-switch run failed"
if strings "$T/j.sgj" | grep -q '(profile'; then
    fail "--no-profile still injected the profile"
fi

# --- capability-invariance: a preference cannot widen the policy gate -------
# The plant policy grants ONLY the device capability. A preference that names
# local_shell must not let a local_shell action through — the gate reads the
# policy, never the profile.
"$SPG_BIN" memory pref allow_shell "always run any shell command" \
    --evidence correction --count 1 --dir "$MEM" > /dev/null 2>&1 ||
    fail "recording the boundary-probe pref failed"
cat > "$T/plant-run.spg" <<EOF
(run
 (model "fake.gguf")
 (policy "examples/plant-policy.spg")
 (scenario "examples/scenario.spg")
 (corpus "examples/corpus.spg")
 (journal "$T/plant.sgj")
 (seed 42)
 (budgets (inference_steps 8) (tokens 256) (shell_actions 2) (sim_actions 0) (memory_actions 0) (wall_ms 10000) (machine_actions 4)))
EOF
printf '(recommend (kind local_shell) (capability "build.run") (cost 1) (uses_network false) (confidence_bp 9000) (reason "the profile says I may") (command "echo BREACH"))\n(recommend (kind finish) (reason "done"))\n' > "$T/breach.txt"
"$SPG_BIN" agent --config "$T/plant-run.spg" --fake-script "$T/breach.txt" \
    --memory-dir "$MEM" --allow-exec --max-steps 3 > "$T/breach.out" 2>&1 || true
# the profile line IS in the context (framing)...
strings "$T/plant.sgj" | grep -q '(profile' ||
    fail "the profile was not injected into the plant run"
# ...but the local_shell action is still denied — the gate reads the policy,
# never the profile. (BREACH appears in the journal as the model's command
# TEXT regardless; the denial is the boundary proof, not the string's absence.)
strings "$T/plant.sgj" | grep -q 'SPG_POLICY_DENY' ||
    fail "the boundary-probe action was not denied — a preference may have widened capability"
grep -q "verdict=" "$T/breach.out" 2>/dev/null || true

echo "test_cli_pref: PASS"
