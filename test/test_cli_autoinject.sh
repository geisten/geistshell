#!/bin/sh
# System test: slug-triggered lesson auto-injection (docs/LEARNING.md P6).
# When a stored lesson names the current failure slug, the loop leads the
# repair observation with its earned directive — no memory_read needed. With
# no such lesson, only the generic hint appears. Model-free (fake script).
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}
CHAT=$(dirname "$SPG_BIN")/geistshell-chat
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

# a script that stays malformed: repair fires once, then terminates rejected,
# leaving the repair observation (with any directive) in the buffer
cat > "$T/bad.txt" <<'EOF'
not a recommendation
still not a recommendation
EOF

MEM=$(mktemp -d)
# Seed a lesson-rejected via improve over the shipped gated suite.
"$SPG_BIN" improve examples/eval/improve_gated.spg --memory-dir "$MEM" >/dev/null 2>&1
test -f "$MEM/lesson-rejected.md"

# WITH the lesson in the store: the directive (its description) leads the
# repair observation.
"$SPG_BIN" agent --config "$T/run.spg" --fake-script "$T/bad.txt" \
    --memory-dir "$MEM" --max-steps 5 --max-repairs 1 > "$T/with.out" 2>&1
grep -q "termination=rejected" "$T/with.out"
grep -q "Emit exactly one valid recommendation s-expression" "$T/with.out"

# WITHOUT a lesson (empty memory dir): only the generic hint, no directive.
MT=$(mktemp -d)
"$SPG_BIN" agent --config "$T/run.spg" --fake-script "$T/bad.txt" \
    --memory-dir "$MT" --max-steps 5 --max-repairs 1 > "$T/without.out" 2>&1
grep -q "termination=rejected" "$T/without.out"
! grep -q "Emit exactly one valid recommendation s-expression" "$T/without.out"

rm -rf "$MEM" "$MT"
echo "test_cli_autoinject: PASS"
