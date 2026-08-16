#!/bin/sh
# System test: --best-of N selects without an oracle (geistshell#55).
#
# The defect: selection went through spg_eval_judge, which needs a declared
# (expect ...) — the ANSWER. Without one, `attempts = have_expect ? best_of : 1`
# quietly made the flag a no-op, so the shipped feature was off in production,
# where nobody knows the expected observation. The 5/5-vs-0/5 it was measured
# at was obtained knowing the answer.
#
# Scripted fakes throughout: what is under test is the SELECTION, and the
# selector reads only termination and status.
set -eu
SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
die() { echo "FAIL: $*" >&2; exit 1; }
mkrun() { # mkrun <name> [extra run fields]
    cat > "$T/$1.spg" <<EOF
(run
 (model "fake.gguf")
 (policy "examples/policy.spg")
 (scenario "examples/scenario.spg")
 (corpus "examples/corpus.spg")
 (journal "$T/$1.sgj")
 (seed 42)
 $2
 (budgets
  (inference_steps 8) (tokens 256) (shell_actions 1) (sim_actions 8)
  (memory_actions 8) (wall_ms 10000)))
EOF
}
# ---- 1. best-of runs at all without an (expect ...) ------------------------
mkrun plain ""
"$SPG_BIN" agent --config "$T/plain.spg" \
    --fake-script examples/eval/broken.txt --max-repairs 0 --best-of 3 \
    > "$T/plain.out" 2>&1 || true
grep -q 'best_of=3' "$T/plain.out" ||
    die "best-of must report itself without an expectation: $(cat "$T/plain.out")"
grep -q 'attempts_used=3' "$T/plain.out" ||
    die "a run that never reaches the top rung must use every attempt"
grep -q 'rank=0' "$T/plain.out" ||
    die "a rejected run should be selected at the bottom rung"
# ---- 2. it stops at the top rung instead of burning attempts --------------
"$SPG_BIN" agent --config "$T/plain.spg" \
    --fake-script examples/eval/sim_finish.txt --best-of 5 \
    > "$T/top.out" 2>&1 || die "a finishing script must succeed"
grep -q 'attempts_used=1' "$T/top.out" ||
    die "a finished run cannot be improved on; stop at one attempt"
grep -q 'rank=4' "$T/top.out" || die "finished is the top answer-free rung"
grep -q 'termination=finished' "$T/top.out" || die "termination should be finished"
# ---- 3. the selected run is the one REPORTED ------------------------------
# The observation and termination printed must belong to the chosen attempt,
# not to whichever attempt happened to run last.
grep -q 'chosen=1' "$T/top.out" || die "the chosen attempt must be reported"
# ---- 4. an (expect ...) still wins when present ---------------------------
mkrun expected '(expect "patch_vulnerability")'
"$SPG_BIN" agent --config "$T/expected.spg" \
    --fake-script examples/eval/sim_finish.txt --best-of 4 \
    > "$T/expected.out" 2>&1 || die "a satisfied expectation must exit zero"
grep -q 'verdict=pass' "$T/expected.out" || die "the expectation should pass"
grep -q 'attempts_used=1' "$T/expected.out" ||
    die "a passing attempt is unbeatable; stop immediately"
grep -q 'rank=100' "$T/expected.out" ||
    die "a satisfied expectation must outrank every answer-free rung"
# ---- 5. an unsatisfiable expectation still uses every attempt -------------
mkrun unmet '(expect "THIS-NEVER-APPEARS")'
if "$SPG_BIN" agent --config "$T/unmet.spg" \
        --fake-script examples/eval/sim_finish.txt --best-of 3 \
        > "$T/unmet.out" 2>&1; then
    die "a failed expectation must exit non-zero"
fi
grep -q 'attempts_used=3' "$T/unmet.out" ||
    die "an unmet expectation should exhaust the attempts"
grep -q 'verdict=fail_observation' "$T/unmet.out" ||
    die "the verdict should say which expectation failed"
# ... and it falls back to the answer-free rung rather than reporting nothing
grep -q 'rank=4' "$T/unmet.out" ||
    die "with no attempt passing, the best answer-free run is selected"
# ---- 6. --best-of 1 is unchanged ------------------------------------------
# The single-attempt path must stay byte-identical, or every existing script
# and recorded benchmark silently changes shape.
"$SPG_BIN" agent --config "$T/plain.spg" \
    --fake-script examples/eval/sim_finish.txt > "$T/one.out" 2>&1 ||
    die "a plain run must still work"
grep -q 'best_of=' "$T/one.out" &&
    die "a single attempt must not print best-of bookkeeping"
"$SPG_BIN" agent --config "$T/plain.spg" \
    --fake-script examples/eval/sim_finish.txt --best-of 1 > "$T/one2.out" 2>&1 ||
    die "--best-of 1 must work"
diff "$T/one.out" "$T/one2.out" > /dev/null ||
    die "--best-of 1 must be identical to no flag at all"
echo "test_cli_best_of: PASS"