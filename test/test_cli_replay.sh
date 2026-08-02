#!/bin/sh
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}
JOURNAL=build/tick-demo.sgj
OUT=build/test-cli-replay.jsonl

"$SPG_BIN" run \
    --config examples/run.spg \
    --fake '(recommend (kind simulator) (capability "sim.act") (cost 1) (uses_network false) (confidence_bp 9000) (reason "reduce simulated risk"))' \
    --ticks 1 \
    >/dev/null

"$SPG_BIN" replay "$JOURNAL" >"$OUT"

python3 - "$OUT" <<'PY'
import json
import sys

path = sys.argv[1]
records = []
with open(path, encoding="utf-8") as f:
    for line in f:
        records.append(json.loads(line))

assert len(records) == 8, len(records)
assert records[0]["event"] == "model_input"
policy = [r for r in records if r["event"] == "policy_decision"]
sim = [r for r in records if r["event"] == "sim"]
assert len(policy) == 1, policy
assert policy[0]["decision"] == "allow", policy[0]
assert policy[0]["action_kind"] == "simulator", policy[0]
assert len(sim) == 1, sim
assert sim[0]["action"] == "patch_vulnerability", sim[0]
assert sim[0]["risk_after"] < sim[0]["risk_before"], sim[0]
PY
