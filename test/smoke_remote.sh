#!/bin/sh
# Smoke test: the REMOTE model adapter end-to-end through the real libcurl path.
# A local mock serves an OpenAI-compatible /v1/chat/completions response; the
# governed `run` loop drives it and journals the resulting action.
#
# NOT auto-run by `make test` (named smoke_*). Needs a REMOTE=1 build and
# python3; both are detected and the test SKIPs cleanly when absent.
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}

if ! command -v python3 >/dev/null 2>&1; then
    echo "smoke_remote: SKIP (no python3)"
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

# --- mock OpenAI server: returns one valid recommendation, prints its port ---
cat > "$T/mock.py" <<'PY'
import http.server, json, sys
REC = ('(recommend (kind simulator) (capability "sim.act") (cost 1) '
       '(uses_network false) (confidence_bp 9000) (reason "smoke"))')
class H(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        ln = int(self.headers.get('Content-Length', 0)); self.rfile.read(ln)
        body = json.dumps({"choices": [{"index": 0, "message": {
            "role": "assistant", "content": REC}, "finish_reason": "stop"}],
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

# The mock writes its port only after bind()+listen(), so a non-empty port file
# means the server is already accepting connections. Poll for it (perl gives a
# sub-second sleep without relying on a `sleep` binary).
PORT=""
i=0
while [ "$i" -lt 100 ]; do
    if [ -s "$T/port" ]; then
        PORT=$(cat "$T/port")
        break
    fi
    perl -e 'select(undef,undef,undef,0.1)'
    i=$((i + 1))
done
if [ -z "$PORT" ]; then
    echo "smoke_remote: FAIL (mock server did not start)" >&2
    exit 1
fi

cat > "$T/run.spg" <<EOF
(run
 (model "mock-model")
 (policy "examples/policy.spg")
 (scenario "examples/scenario.spg")
 (corpus "examples/corpus.spg")
 (journal "$T/j.sgj")
 (seed 42)
 (budgets (inference_steps 4) (tokens 256) (shell_actions 0) (sim_actions 4) (memory_actions 4) (wall_ms 10000)))
EOF

# bound the run so a hang never wedges the suite (macOS has no timeout(1))
set +e
OUT=$(perl -e 'alarm 30; exec @ARGV' "$SPG_BIN" run --config "$T/run.spg" \
        --remote-url "http://127.0.0.1:$PORT/v1/chat/completions" \
        --remote-model "mock-model" --ticks 1 2>&1)
RC=$?
set -e

# A binary built without REMOTE=1 cannot run this path — skip rather than fail.
case "$OUT" in
    *"make REMOTE=1"*)
        echo "smoke_remote: SKIP (binary built without REMOTE=1)"
        exit 0
        ;;
esac

if [ "$RC" -ne 0 ]; then
    echo "smoke_remote: FAIL (run exit $RC)" >&2
    printf '%s\n' "$OUT" >&2
    exit 1
fi
printf '%s\n' "$OUT" | grep -q "model init failed" && {
    echo "smoke_remote: FAIL (model init failed)" >&2; exit 1; }
test -s "$T/j.sgj" || { echo "smoke_remote: FAIL (no journal)" >&2; exit 1; }

# the remote-driven action must appear in the journal
"$SPG_BIN" replay "$T/j.sgj" | grep -qi "sim" || {
    echo "smoke_remote: FAIL (no simulator action journaled)" >&2
    "$SPG_BIN" replay "$T/j.sgj" >&2
    exit 1
}

echo "smoke_remote: PASS"
