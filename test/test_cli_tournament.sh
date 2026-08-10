#!/bin/sh
# Roadmap phase 10 (#70): the tournament runner.
#
# What a smoke test can prove here is not that a model is good — it is that the
# table cannot lie: a model that did not run must be visible as such, and the
# split between known and held-out must survive into the output.
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}
OUT_DIR=build/tournament-test
RUNNER=examples/machine/run_tournament.sh

export SPG_BIN
rm -rf "$OUT_DIR"

sh "$RUNNER" --models "fake geist:/nonexistent.gguf" --out-dir "$OUT_DIR" \
    >/dev/null 2>&1

python3 - "$OUT_DIR/machine-benchmark.jsonl" "$OUT_DIR/machine-benchmark.md" <<'PY'
import json, sys

rows = [json.loads(l) for l in open(sys.argv[1], encoding="utf-8")]
by = {r["model"]: r for r in rows}

fake = by["fake"]
assert fake["available"] is True, fake
# The scripted control must score perfectly, or the harness is not measuring
# what it claims to measure.
assert fake["known"] == fake["known_runs"], fake
assert fake["heldout"] == fake["heldout_runs"], fake
# Known and held-out stay separate all the way into the record.
assert fake["known_runs"] and fake["heldout_runs"], fake
assert "latency_ms" in fake and fake["latency_ms"] is not None, fake
assert fake["peak_rss_kb"], fake

# A model that could not run is a row, not an absence. A table missing an
# entrant reads as "it was fine".
missing = by["geist"]
assert missing["available"] is False, missing
assert missing["status"] != 0, missing
assert missing["known"] is None, missing

# Rejections and denials are different failures and must not be merged.
assert "reject_rate" in fake and "deny_rate" in fake, fake

# The bug this table is most likely to hide: a model entrant that quietly ran
# the scripted answers. It produced a row reading 6/6 in 0 ms for a 3 GB model
# and looked entirely reasonable. The runner now refuses a model suite with no
# model cases, and the smoke test pins that refusal.
md = open(sys.argv[2], encoding="utf-8").read()
assert "| Model | Size | Known | Held-out |" in md, md[:400]
assert "not run" in md, md[:400]
PY

echo "test_cli_tournament: PASS"

# A model suite with no (model ...) cases must be refused, not scored: running
# the scripted answers under a model's name is the failure mode that produces a
# plausible table nobody questions.
cp examples/eval/machine/diagnosis.spg "$OUT_DIR/scripted-as-model.spg"
touch "$OUT_DIR/pretend.gguf"
sh "$RUNNER" --models "geist:$OUT_DIR/pretend.gguf" --out-dir "$OUT_DIR" \
    --model-suite "$OUT_DIR/scripted-as-model.spg" >/dev/null 2>&1 || true

python3 - "$OUT_DIR/machine-benchmark.jsonl" <<'PY'
import json, sys

rows = [json.loads(l) for l in open(sys.argv[1], encoding="utf-8")]
geist = [r for r in rows if r["model"] == "geist"][-1]
assert geist["available"] is False, geist
assert geist["known"] is None, geist
PY

echo "test_cli_tournament: PASS (refusal checked)"
