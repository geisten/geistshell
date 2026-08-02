#!/bin/sh
# System test: the longitudinal benefit audit (docs/LEARNING.md P7). A real
# run that keeps rejecting journals the failure; audit tallies the recurrence
# per slug and flags a kept lesson whose slug still recurs. Model-free.
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

cat > "$T/run.spg" <<EOF
(run
 (model "fake.gguf")
 (policy "examples/policy.spg")
 (scenario "examples/scenario.spg")
 (corpus "examples/corpus.spg")
 (journal "$T/j.sgj")
 (seed 42)
 (budgets (inference_steps 8) (tokens 256) (shell_actions 1) (sim_actions 8) (wall_ms 10000) (journal_bytes 1048576) (risk_bp 10000)))
EOF

# a run that rejects (malformed reply): the journal records the rejection
cat > "$T/bad.txt" <<'EOF'
not a recommendation
still not a recommendation
EOF
"$SPG_BIN" agent --config "$T/run.spg" --fake-script "$T/bad.txt" \
    --max-steps 5 --max-repairs 1 >/dev/null 2>&1

# audit the journal: the rejection slug recurs at least once
OUT=$("$SPG_BIN" audit "$T/j.sgj")
echo "$OUT" | grep -q '"lesson-rejected":[1-9]'

# with a kept lesson-rejected in memory, the audit flags it "review" (still
# recurring)
MEM=$(mktemp -d)
"$SPG_BIN" improve examples/eval/improve_gated.spg --memory-dir "$MEM" >/dev/null 2>&1
test -f "$MEM/lesson-rejected.md"
OUT2=$("$SPG_BIN" audit "$T/j.sgj" --memory-dir "$MEM")
echo "$OUT2" | grep -q '"lesson":"lesson-rejected","recurrence":[1-9][0-9]*,"verdict":"review"'

# a clean run (finish) journals no rejection -> a fresh lesson reads "kept"
cat > "$T/good.txt" <<'EOF'
(recommend (kind finish) (reason "done"))
EOF
rm -f "$T/j.sgj"
"$SPG_BIN" agent --config "$T/run.spg" --fake-script "$T/good.txt" \
    --max-steps 5 >/dev/null 2>&1
OUT3=$("$SPG_BIN" audit "$T/j.sgj" --memory-dir "$MEM")
echo "$OUT3" | grep -q '"lesson":"lesson-rejected","recurrence":0,"verdict":"kept"'

rm -rf "$MEM"
echo "test_cli_audit: PASS"
