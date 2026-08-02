#!/bin/sh
# Smoke test: the eval/improve REMOTE path end-to-end through real libcurl.
# A local mock serves an OpenAI-compatible model that ADVANCES — a simulator
# action, then a finish — so a multi-step governed case actually terminates.
# Asserts the path works (JSONL emitted, including the --samples aggregate and
# the improve summary), not that the small model "passes" (cf. smoke_eval_real).
#
# NOT auto-run (smoke_*). Needs a REMOTE=1 build + python3; both detected, SKIPs
# cleanly when absent.
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}

if ! command -v python3 >/dev/null 2>&1; then
    echo "smoke_eval_remote: SKIP (no python3)"
    exit 0
fi

T=$(mktemp -d)
cleanup() {
    if [ -n "${MOCK_PID:-}" ]; then
        kill "$MOCK_PID" 2>/dev/null || true
        wait "$MOCK_PID" 2>/dev/null || true
    fi
    rm -rf "$T"
}
trap cleanup EXIT

# Mock advances: even POST -> a simulator action, odd POST -> finish. Each run
# is therefore one gated+journaled action then a clean finish; bounded anyway by
# the case's max_steps.
cat > "$T/mock.py" <<'PY'
import http.server, json, sys
ACTION = ('(recommend (kind simulator) (capability "sim.act") (cost 1) '
          '(uses_network false) (confidence_bp 9000) (reason "smoke"))')
FINISH = '(recommend (kind finish) (reason "done"))'
count = 0
class H(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        global count
        ln = int(self.headers.get('Content-Length', 0)); self.rfile.read(ln)
        content = ACTION if (count % 2 == 0) else FINISH
        count += 1
        body = json.dumps({"choices": [{"index": 0, "message": {
            "role": "assistant", "content": content}, "finish_reason": "stop"}],
            "usage": {"completion_tokens": 5}}).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers(); self.wfile.write(body)
    def log_message(self, *a): pass
s = http.server.HTTPServer(('127.0.0.1', 0), H)
with open(sys.argv[1], 'w') as f: f.write(str(s.server_address[1]))
s.serve_forever()
PY

python3 "$T/mock.py" "$T/port" &
MOCK_PID=$!

PORT=""
i=0
while [ "$i" -lt 100 ]; do
    if [ -s "$T/port" ]; then PORT=$(cat "$T/port"); break; fi
    perl -e 'select(undef,undef,undef,0.1)'
    i=$((i + 1))
done
if [ -z "$PORT" ]; then
    echo "smoke_eval_remote: FAIL (mock did not start)" >&2
    exit 1
fi
URL="http://127.0.0.1:$PORT/v1/chat/completions"

cat > "$T/suite.spg" <<EOF
(eval_suite
 (config "examples/run.spg")
 (case (name "remote-sim") (model "remote") (max_steps 4)
       (expect (termination finished))))
EOF

run_eval() { # extra-args...  (a non-zero exit is fine: we assert on output)
    perl -e 'alarm 30; exec @ARGV' "$SPG_BIN" eval "$T/suite.spg" \
        --remote-url "$URL" --remote-model "mock-model" "$@" 2>&1 || true
}

OUT=$(run_eval)
case "$OUT" in
    *"make REMOTE=1"*)
        echo "smoke_eval_remote: SKIP (binary built without REMOTE=1)"
        exit 0
        ;;
esac

# the path must emit a per-case verdict and a suite summary
printf '%s\n' "$OUT" | grep -q '"name":"remote-sim","outcome":' || {
    echo "smoke_eval_remote: FAIL (no case verdict)" >&2
    printf '%s\n' "$OUT" >&2; exit 1; }
printf '%s\n' "$OUT" | grep -q '"suite":' || {
    echo "smoke_eval_remote: FAIL (no suite summary)" >&2; exit 1; }

# --samples 2 must emit the k-of-N aggregate fields
OUT2=$(run_eval --samples 2)
printf '%s\n' "$OUT2" | grep -q '"runs":2,"passed":' || {
    echo "smoke_eval_remote: FAIL (no samples aggregate)" >&2
    printf '%s\n' "$OUT2" >&2; exit 1; }

# improve must complete and emit its final summary line
OUT3=$(perl -e 'alarm 60; exec @ARGV' "$SPG_BIN" improve "$T/suite.spg" \
        --remote-url "$URL" --remote-model "mock-model" \
        --memory-dir "$T/mem" 2>&1 || true)
printf '%s\n' "$OUT3" | grep -q '"final_passed":' || {
    echo "smoke_eval_remote: FAIL (improve emitted no final summary)" >&2
    printf '%s\n' "$OUT3" >&2; exit 1; }

echo "smoke_eval_remote: PASS"
