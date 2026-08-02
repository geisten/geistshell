#!/bin/sh
# A (model "remote") case with no endpoint configured must fail CLEANLY
# (outcome fail_run_error, non-zero suite exit) — never crash or abort the
# whole suite. This exercises the eval REMOTE branch's guard and runs in the
# default build (no libcurl needed; the guard fires before any HTTP).
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}
unset GEISTSHELL_API_URL GEISTSHELL_API_KEY 2>/dev/null || true

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

cat > "$T/remote.spg" <<EOF
(eval_suite
 (config "examples/run.spg")
 (case (name "remote-case") (model "remote") (max_steps 3)
       (expect (termination finished))))
EOF

# No endpoint -> the case reports a run-error and the suite exits non-zero.
if "$SPG_BIN" eval "$T/remote.spg" > "$T/out" 2>"$T/err"; then
    echo "test_cli_eval_remote: FAIL (expected non-zero exit)" >&2
    cat "$T/out" >&2
    exit 1
fi
grep -q '"name":"remote-case","outcome":"fail_run_error"' "$T/out" || {
    echo "test_cli_eval_remote: FAIL (no fail_run_error verdict)" >&2
    cat "$T/out" >&2
    exit 1
}
grep -q '"suite":' "$T/out" || {
    echo "test_cli_eval_remote: FAIL (no suite summary)" >&2
    exit 1
}

echo "test_cli_eval_remote: PASS"
