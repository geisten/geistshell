#!/bin/sh
# Roadmap phase 11 (#71): the ablation runner.
#
# A smoke test cannot say which fields a model needs. It can say that the
# experiment is capable of showing it: every variant must actually shrink the
# context, the floor must be a floor, and an unrecognised mask must fail rather
# than quietly run the full context under an ablation's name.
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}
OUT=build/ablation-test.jsonl

export SPG_BIN
sh examples/machine/run_ablation.sh --out "$OUT" >/dev/null 2>&1

python3 - "$OUT" <<'PY'
import json, sys

rows = {r["variant"]: r for r in
        (json.loads(l) for l in open(sys.argv[1], encoding="utf-8"))}

full = rows["full"]["context_bytes_mean"]
assert full, rows["full"]

# Every variant removes something, or it is not a variant.
for name, row in rows.items():
    if name == "full":
        continue
    assert row["context_bytes_mean"] < full, (name, row)

# The floor is a floor: nothing else may be smaller.
bare = rows["bare"]["context_bytes_mean"]
for name, row in rows.items():
    assert row["context_bytes_mean"] >= bare, (name, row)

# The scripted control ignores the context entirely, so it must score the same
# everywhere. A control that moves means the harness is measuring itself.
scores = {(r["known"], r["heldout"]) for r in rows.values()}
assert len(scores) == 1, rows
PY

# An unrecognised mask must not silently become "no ablation" — a row labelled
# no_temperature that actually ran the full context is worse than no row.
if "$SPG_BIN" eval examples/eval/machine/diagnosis.spg --ablate nonsense \
        >/dev/null 2>&1; then
    echo "test_cli_ablation: FAIL — an unknown --ablate spec was accepted" >&2
    exit 1
fi

echo "test_cli_ablation: PASS"
