#!/bin/sh
# System test: the optional success criterion (docs/LEARNING.md P1). A run
# config carrying (expect "<substring>") is judged after the governed loop
# finishes — model-free, the world's observation supplies the verdict. A
# matching substring passes; a non-matching one fails and exits non-zero.
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

# The shell step echoes a marker that reaches the observation channel.
cat > "$T/script.txt" <<'EOF'
(recommend (kind local_shell) (capability "build.run") (cost 1) (uses_network false) (confidence_bp 6000) (reason "probe") (command "echo VERIFIER-MARKER"))
(recommend (kind finish) (reason "task complete"))
EOF

make_config() {  # $1 = journal, $2 = expect substring
    cat > "$T/run.spg" <<EOF
(run
 (model "fake.gguf")
 (policy "examples/policy.spg")
 (scenario "examples/scenario.spg")
 (corpus "examples/corpus.spg")
 (journal "$1")
 (seed 42)
 (budgets (inference_steps 8) (tokens 256) (shell_actions 1) (sim_actions 8) (wall_ms 10000) (journal_bytes 1048576) (risk_bp 10000))
 (expect "$2"))
EOF
}

# --- criterion met: the observation contains the marker -> verdict pass, rc 0 ---
make_config "$T/j.sgj" "VERIFIER-MARKER"
rc=0
"$SPG_BIN" agent --config "$T/run.spg" --fake-script "$T/script.txt" \
    --allow-exec --max-steps 5 > "$T/pass.out" 2>&1 || rc=$?
grep -q "verdict=pass" "$T/pass.out"
[ "$rc" -eq 0 ]

# --- criterion NOT met: marker absent from the observation -> fail, rc non-zero ---
rm -f "$T/j.sgj"
make_config "$T/j.sgj" "MARKER-THAT-NEVER-APPEARS"
rc=0
"$SPG_BIN" agent --config "$T/run.spg" --fake-script "$T/script.txt" \
    --allow-exec --max-steps 5 > "$T/fail.out" 2>&1 || rc=$?
grep -q "verdict=fail" "$T/fail.out"
[ "$rc" -ne 0 ]

# --- no (expect) field: behaves exactly as before, no verdict line ---
rm -f "$T/j.sgj"
cat > "$T/run_noexpect.spg" <<EOF
(run
 (model "fake.gguf")
 (policy "examples/policy.spg")
 (scenario "examples/scenario.spg")
 (corpus "examples/corpus.spg")
 (journal "$T/j.sgj")
 (seed 42)
 (budgets (inference_steps 8) (tokens 256) (shell_actions 1) (sim_actions 8) (wall_ms 10000) (journal_bytes 1048576) (risk_bp 10000)))
EOF
"$SPG_BIN" agent --config "$T/run_noexpect.spg" --fake-script "$T/script.txt" \
    --allow-exec --max-steps 5 > "$T/noexpect.out" 2>&1
grep -q "termination=finished" "$T/noexpect.out"
! grep -q "verdict=" "$T/noexpect.out"

echo "test_cli_expect: PASS"
