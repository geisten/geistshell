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

# #53: the parse/gate/task ladder. A single pass-rate cannot tell "the model
# cannot produce the form" from "it produces a valid form and picks a
# forbidden action", and those want opposite fixes. The rungs are monotone:
# task <= gate <= parse.
printf '%s\n' "$OUT" | grep -q '"name":"sim-finish".*"parsed":1,"gated":1'

# #126: per-case cost. Tokens are deterministic for scripted fakes (one per
# tick, so a 2-step case is 2) and live in the default output; wall time
# varies, so wall_ms appears only under --timing, next to latency_ms.
printf '%s\n' "$OUT" | grep -q '"name":"sim-finish".*"tokens":2'
if printf '%s\n' "$OUT" | grep -q '"wall_ms":'; then
    echo "wall_ms leaked into the default (deterministic) output" >&2
    exit 1
fi
"$SPG_BIN" eval --timing examples/eval/suite.spg | grep -q '"name":"sim-finish".*"tokens":2,"wall_ms":'

# a reply that never parses earns NO rung
cat > "$T/reject.spg" <<EOF
(eval_suite
 (config "examples/run.spg")
 (case (name "never-parses") (script "examples/eval/broken.txt") (max_steps 2)
       (expect (termination rejected))))
EOF
"$SPG_BIN" eval "$T/reject.spg" > "$T/reject.out" 2>&1 || true
grep -q '"name":"never-parses","outcome":"pass","termination":"rejected"' "$T/reject.out" || {
    echo "test setup: expected a rejected termination" >&2
    cat "$T/reject.out" >&2
    exit 1
}
grep -q '"parsed":0,"gated":0' "$T/reject.out" || {
    echo "FAIL: a rejected run must earn no ladder rung" >&2
    cat "$T/reject.out" >&2
    exit 1
}
grep -q '"suite":.*"parsed":0,"gated":0' "$T/reject.out" || {
    echo "FAIL: the suite summary must carry the ladder too" >&2
    exit 1
}

# a form that parses but is refused by the policy gate earns the parse rung
# and stops there — the case passes its expectation while task-rate is 0.
cat > "$T/denied.txt" <<'EOS'
(recommend (kind ssh_auth_probe) (capability "auth_probe.ssh_publickey_single") (cost 1) (uses_network true) (confidence_bp 6000) (reason "probe") (target "db-01"))
EOS
cat > "$T/denied.spg" <<EOF
(eval_suite
 (config "examples/run.spg")
 (case (name "gate-denies") (script "$T/denied.txt") (max_steps 2)
       (expect (termination denied))))
EOF
"$SPG_BIN" eval "$T/denied.spg" > "$T/denied.out" 2>&1 || true
grep -q '"termination":"denied"' "$T/denied.out" || {
    echo "test setup: expected a denied termination" >&2
    cat "$T/denied.out" >&2
    exit 1
}
grep -q '"parsed":1,"gated":0' "$T/denied.out" || {
    echo "FAIL: a denied run must earn the parse rung but not the gate rung" >&2
    cat "$T/denied.out" >&2
    exit 1
}

# #128: the answer-judged column, orthogonal to the ladder. A model that KNOWS
# the answer but says it in prose scores parse 0 — before this column that was
# indistinguishable from not knowing the answer at all.
#
# Free-arm signature: the raw reply contains the expected substring, the form
# never parses -> answered:1, parsed:0.
cat > "$T/prose.txt" <<'EOS'
The count is sandbox_result_7, no action needed.
EOS
cat > "$T/prose.spg" <<EOF
(eval_suite
 (config "examples/run.spg")
 (case (name "knows-prose") (script "$T/prose.txt") (max_steps 2)
       (expect (observation "sandbox_result_7"))))
EOF
"$SPG_BIN" eval "$T/prose.spg" > "$T/prose.out" 2>&1 || true
grep -q '"name":"knows-prose".*"parsed":0.*"answered":1' "$T/prose.out" || {
    echo "FAIL: a prose reply containing the answer must score answered without parse" >&2
    cat "$T/prose.out" >&2
    exit 1
}

# Action-path control: exec-finish passes through a parsed action whose
# OBSERVATION carries the marker; the raw reply itself does not -> answered:0.
printf '%s\n' "$OUT" | grep -q '"name":"exec-finish".*"answered":0' || {
    echo "FAIL: answered must judge the raw reply, not the action observation" >&2
    exit 1
}

# A case with no (expect (observation ...)) must not grow the column at all.
if printf '%s\n' "$OUT" | grep -q '"name":"sim-finish".*"answered"'; then
    echo "FAIL: answered leaked into a case with no declared answer" >&2
    exit 1
fi

echo "test_cli_eval: PASS"
