#!/bin/sh
# Smoke test (NOT run by `make test` — needs a GGUF and the geistd binary):
# geistshell against a resident daemon. Starts geistd on a private socket,
# runs the adapter test's geistd path (free + constrained decode), then the
# real eval path with GEISTSHELL_GEISTD set, which is how an agent run uses it.
#   GEISTD_BIN=/opt/homebrew/bin/geistd GEISTSHELL_MODEL=/path/model.gguf sh test/smoke_geistd.sh
set -eu
GEISTD_BIN=${GEISTD_BIN:-geistd}
MODEL=${GEISTSHELL_MODEL:-gguf_artifacts/gemma4-e2b-Q4_K_M.gguf}
command -v "$GEISTD_BIN" >/dev/null 2>&1 || { echo "smoke_geistd: SKIP (no geistd binary: $GEISTD_BIN)"; exit 0; }
[ -f "$MODEL" ] || { echo "smoke_geistd: SKIP (model not found: $MODEL)"; exit 0; }
SOCK=/tmp/geistshell-geistd-$$.sock; LOG=/tmp/geistshell-geistd-$$.log
"$GEISTD_BIN" "$MODEL" --socket "$SOCK" --sessions 4 2>"$LOG" & D=$!
trap 'kill $D 2>/dev/null || true; wait $D 2>/dev/null || true; rm -f "$SOCK" "$LOG"' EXIT
i=0; while ! grep -q listening "$LOG" 2>/dev/null && [ $i -lt 60 ]; do sleep 1; i=$((i+1)); done
grep -q listening "$LOG" || { echo "smoke_geistd: geistd did not start: $(tail -2 "$LOG")"; exit 1; }
echo "smoke_geistd: adapter path"
GEISTSHELL_TEST_GEISTD="$SOCK" ./build/host-debug/test/test_model_adapter
echo "smoke_geistd: eval path through the daemon"
GEISTSHELL_GEISTD="$SOCK" GEISTSHELL_MODEL="$MODEL" sh test/smoke_eval_real.sh
echo "smoke_geistd: OK"
