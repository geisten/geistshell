#!/bin/sh
# System test: the eval harness scores a scripted agent suite and emits a JSONL
# report (the machine-readable signal a self-improvement loop consumes).
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}

OUT=$("$SPG_BIN" eval examples/eval/suite.spg)
printf '%s\n' "$OUT"

# every case passed, with the expected termination per case
printf '%s\n' "$OUT" | grep -q '"name":"sim-finish","outcome":"pass","termination":"finished","steps":2'
printf '%s\n' "$OUT" | grep -q '"name":"exec-finish","outcome":"pass","termination":"finished"'
printf '%s\n' "$OUT" | grep -q '"name":"capped","outcome":"pass","termination":"max_steps"'
printf '%s\n' "$OUT" | grep -q '"suite":"examples/eval/suite.spg","total":3,"passed":3'

# a failing expectation is reported (not crashed): point a case at the wrong
# termination and confirm the runner flags it and exits non-zero.
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cat > "$T/bad.spg" <<EOF
(eval_suite
 (config "examples/run.spg")
 (case (name "wrong") (script "examples/eval/sim_finish.txt") (max_steps 5)
       (expect (termination budget))))
EOF
if "$SPG_BIN" eval "$T/bad.spg" > "$T/bad.out" 2>&1; then
    echo "expected non-zero exit for a failing suite" >&2
    exit 1
fi
grep -q '"name":"wrong","outcome":"fail_termination"' "$T/bad.out"

# #51: --constrained / --temperature exist and leave scripted-fake suites
# byte-identical. They only reach (model "geist") cases, so a fake suite must
# produce exactly the same report with and without them — that is what makes it
# safe to turn them on by default in a bench preset later.
OUT2=$("$SPG_BIN" eval examples/eval/suite.spg --constrained --temperature 0.8)
[ "$OUT" = "$OUT2" ] || {
    echo "--constrained/--temperature changed a scripted-fake suite" >&2
    printf 'without:\n%s\nwith:\n%s\n' "$OUT" "$OUT2" >&2
    exit 1
}

# a bad temperature is rejected, not silently coerced to NaN inside the adapter
if "$SPG_BIN" eval examples/eval/suite.spg --temperature abc > "$T/temp.out" 2>&1; then
    echo "expected non-zero exit for an invalid --temperature" >&2
    exit 1
fi
grep -q 'invalid --temperature' "$T/temp.out"

echo "test_cli_eval: PASS"
