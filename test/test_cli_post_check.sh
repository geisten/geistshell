#!/bin/sh
# System test: the world-state post-check (docs/LEARNING.md decision 2, #13).
# Some tasks produce a FILE instead of stdout — their success is invisible to
# the (expect ...) substring. A run config carrying (post_check "<cmd>") runs
# the command once after the loop, bounded through cmd_executor, and its
# exit 0 ANDs into the verdict. Model-free, zero tokens: a shell probe.
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

# The task writes a file and says nothing useful on stdout.
cat > "$T/script.txt" <<EOF
(recommend (kind local_shell) (capability "build.run") (cost 1) (uses_network false) (confidence_bp 6000) (reason "produce the artifact") (command "touch $T/report.out"))
(recommend (kind finish) (reason "task complete"))
EOF

make_config() {  # $1 = post_check command, $2 = optional extra form
    cat > "$T/run.spg" <<EOF
(run
 (model "fake.gguf")
 (policy "examples/policy.spg")
 (scenario "examples/scenario.spg")
 (corpus "examples/corpus.spg")
 (journal "$T/j.sgj")
 (seed 42)
 (budgets (inference_steps 8) (tokens 256) (shell_actions 1) (sim_actions 8) (wall_ms 10000))
 ${2:-}
 (post_check "$1"))
EOF
}

# --- the file exists -> post_check pass, verdict pass, rc 0 -----------------
make_config "test -f $T/report.out"
rc=0
"$SPG_BIN" agent --config "$T/run.spg" --fake-script "$T/script.txt" \
    --allow-exec --max-steps 5 > "$T/pass.out" 2>&1 || rc=$?
grep -q "post_check=pass" "$T/pass.out"
grep -q "verdict=pass" "$T/pass.out"
[ "$rc" -eq 0 ]

# --- the probe fails -> verdict fail, rc non-zero ---------------------------
make_config "test -f $T/never-written.out"
rc=0
"$SPG_BIN" agent --config "$T/run.spg" --fake-script "$T/script.txt" \
    --allow-exec --max-steps 5 > "$T/fail.out" 2>&1 || rc=$?
grep -q "post_check=fail" "$T/fail.out"
grep -q "verdict=fail" "$T/fail.out"
[ "$rc" -ne 0 ]

# --- AND-composition: expect passes, post_check fails -> still fail ---------
rm -f "$T/report.out"
cat > "$T/script2.txt" <<EOF
(recommend (kind local_shell) (capability "build.run") (cost 1) (uses_network false) (confidence_bp 6000) (reason "probe") (command "echo POST-MARKER"))
(recommend (kind finish) (reason "done"))
EOF
make_config "test -f $T/never-written.out" '(expect "POST-MARKER")'
rc=0
"$SPG_BIN" agent --config "$T/run.spg" --fake-script "$T/script2.txt" \
    --allow-exec --max-steps 5 > "$T/and.out" 2>&1 || rc=$?
grep -q "post_check=fail" "$T/and.out"
grep -q "verdict=" "$T/and.out"
if grep -q "verdict=pass" "$T/and.out"; then
    echo "FAIL: a failed post_check must overrule a satisfied expect" >&2
    exit 1
fi
[ "$rc" -ne 0 ]

# --- both hold -> pass ------------------------------------------------------
make_config "test -d $T" '(expect "POST-MARKER")'
rc=0
"$SPG_BIN" agent --config "$T/run.spg" --fake-script "$T/script2.txt" \
    --allow-exec --max-steps 5 > "$T/both.out" 2>&1 || rc=$?
grep -q "post_check=pass" "$T/both.out"
grep -q "verdict=pass" "$T/both.out"
[ "$rc" -eq 0 ]

echo "test_cli_post_check: PASS"
