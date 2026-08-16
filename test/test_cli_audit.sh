#!/bin/sh
# System test: the longitudinal benefit audit (docs/LEARNING.md P7). A real
# run that keeps rejecting journals the failure; audit tallies the recurrence
# per slug and splits it at the lesson's mint time, so a run from before the
# lesson existed cannot be held against it. Model-free.
#
# The sleeps are load-bearing: the split is by mtime at one-second granularity
# (see audit_command), so a run and a mint inside the same second would sort
# together.
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
 (budgets (inference_steps 8) (tokens 256) (shell_actions 1) (sim_actions 8) (wall_ms 10000)))
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

# mint the lesson AFTER that run: the rejection is evidence FOR the lesson, not
# against it, so the only journal sorts "before" and the verdict is "pending" --
# no run has yet had the chance to benefit.
sleep 1
MEM=$(mktemp -d)
"$SPG_BIN" improve examples/eval/improve_gated.spg --memory-dir "$MEM" >/dev/null 2>&1
test -f "$MEM/lesson-rejected.md"
OUT2=$("$SPG_BIN" audit "$T/j.sgj" --memory-dir "$MEM")
echo "$OUT2" | grep -q '"before":{"runs":1,"hits":[1-9][0-9]*},"after":{"runs":0,"hits":0},"verdict":"pending"'

# a clean run after the mint: one "after" run, no rejection -> "kept"
sleep 1
cat > "$T/good.txt" <<'EOF'
(recommend (kind finish) (reason "done"))
EOF
"$SPG_BIN" agent --config "$T/run.spg" --fake-script "$T/good.txt" \
    --max-steps 5 >/dev/null 2>&1
OUT3=$("$SPG_BIN" audit "$T/j.sgj" --memory-dir "$MEM")
echo "$OUT3" | grep -q '"before":{"runs":0,"hits":0},"after":{"runs":1,"hits":0},"verdict":"kept"'

# both journals together: the lesson splits them, and the post-mint run still
# rejecting would read "review" rather than being lost in a single sum.
cp "$T/j.sgj" "$T/after.sgj"
OUT4=$("$SPG_BIN" audit "$T/j.sgj" "$T/after.sgj" --memory-dir "$MEM")
echo "$OUT4" | grep -q '"journals":2'
echo "$OUT4" | grep -q '"after":{"runs":2,"hits":0},"verdict":"kept"'

rm -rf "$MEM"
echo "test_cli_audit: PASS"
