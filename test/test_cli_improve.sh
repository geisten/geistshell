#!/bin/sh
# System test: the self-improvement loop. A failing eval case is distilled into
# a mind-palace lesson, persisted, re-evaluated, and kept only if it does not
# regress the suite (the eval harness gates the agent's self-modification).
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

# --- a failing case -> a lesson is learned and persisted ---
OUT=$("$SPG_BIN" improve examples/eval/improve_suite.spg --memory-dir "$T/m1")
printf '%s\n' "$OUT"
printf '%s\n' "$OUT" | grep -q '"lesson":"lesson-rejected","accepted":true'
printf '%s\n' "$OUT" | grep -q '"lessons_kept":1'
# the lesson now lives in the mind-palace (file + index)
test -f "$T/m1/lesson-rejected.md"
grep -q "lesson-rejected" "$T/m1/MEMORY.md"
grep -q "valid (recommend" "$T/m1/lesson-rejected.md"

# --- an all-passing suite learns nothing ---
OUT2=$("$SPG_BIN" improve examples/eval/suite.spg --memory-dir "$T/m2")
printf '%s\n' "$OUT2" | grep -q '"baseline_passed":3,"final_passed":3,"lessons_kept":0'
# no lesson files were created
test -z "$(ls "$T/m2" 2>/dev/null | grep -v MEMORY.md || true)"

# --- measurable gain: a recalled lesson flips a gated case from fail to pass.
#     Baseline rejects (marker absent); after the lesson is saved, its index
#     entry appears in context, the gate opens, and the case finishes. ---
OUT3=$("$SPG_BIN" improve examples/eval/improve_gated.spg --memory-dir "$T/m3")
printf '%s\n' "$OUT3"
printf '%s\n' "$OUT3" | grep -q '"lesson":"lesson-rejected","accepted":true,"baseline_passed":0,"trial_passed":1'
printf '%s\n' "$OUT3" | grep -q '"baseline_passed":0,"final_passed":1,"lessons_kept":1'

# --- hold-out gate: a lesson is distilled from the TRAIN suite but kept only if
#     it improves a separate VALIDATION suite (generalisation, not overfitting).
#     Learn from improve_suite; judge on the held-out improve_gated, where the
#     recalled lesson flips a gated case 0 -> 1. ---
OUT4=$("$SPG_BIN" improve examples/eval/improve_suite.spg \
    --validate examples/eval/improve_gated.spg --memory-dir "$T/h")
printf '%s\n' "$OUT4"
printf '%s\n' "$OUT4" | grep -q '"lesson":"lesson-rejected","accepted":true,"held_out_passed":0,"trial_passed":1'
printf '%s\n' "$OUT4" | grep -q '"validate":"examples/eval/improve_gated.spg"'
printf '%s\n' "$OUT4" | grep -q '"held_out_baseline":0,"held_out_final":1,"lessons_kept":1'

# --- deterministic: two fresh runs of the failing suite agree byte-for-byte ---
"$SPG_BIN" improve examples/eval/improve_suite.spg --memory-dir "$T/a" > "$T/a.out"
"$SPG_BIN" improve examples/eval/improve_suite.spg --memory-dir "$T/b" > "$T/b.out"
cmp "$T/a.out" "$T/b.out"

# --- #27 GEPA-lite: --evolve searches the mutation population against the gate.
#     A suite whose gate marker only the CUE_FIRST wording produces — the
#     concrete reject phrase followed by ". Emit" — cannot be opened by the
#     seed directive, so evolution must adopt the mutation to pass it. ---
cat > "$T/gepa.spg" <<'EOF'
(eval_suite
 (config "examples/run.spg")
 (case (name "gepa") (script "examples/eval/gated.txt") (gate_marker "schema. Emit exactly") (max_steps 5)
       (expect (termination finished))))
EOF
# without --evolve the seed wording cannot open the gate: the case fails
OUTG=$("$SPG_BIN" improve "$T/gepa.spg" --memory-dir "$T/g1")
printf '%s\n' "$OUTG" | grep -q '"trial_passed":0' || {
    echo "FAIL: setup — the seed unexpectedly opened the GEPA gate" >&2
    echo "$OUTG" >&2; exit 1
}
if printf '%s\n' "$OUTG" | grep -q 'evolved'; then
    echo "FAIL: the evolved field leaked without --evolve" >&2; exit 1
fi
# with --evolve the search adopts the CUE_FIRST wording and the case passes
OUTE=$("$SPG_BIN" improve "$T/gepa.spg" --evolve --memory-dir "$T/g2")
printf '%s\n' "$OUTE"
printf '%s\n' "$OUTE" | grep -q '"evolved":"cue_first"' || {
    echo "FAIL: evolution did not adopt the wording that opens the gate" >&2
    echo "$OUTE" >&2; exit 1
}
printf '%s\n' "$OUTE" | grep -q '"trial_passed":1' ||
    { echo "FAIL: the evolved directive did not flip the case" >&2; exit 1; }

# --- --evolve is deterministic: two fresh runs agree byte-for-byte ----------
"$SPG_BIN" improve "$T/gepa.spg" --evolve --memory-dir "$T/e1" > "$T/e1.out"
"$SPG_BIN" improve "$T/gepa.spg" --evolve --memory-dir "$T/e2" > "$T/e2.out"
cmp "$T/e1.out" "$T/e2.out"

echo "test_cli_improve: PASS"
