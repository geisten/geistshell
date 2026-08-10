#!/bin/sh
# Roadmap phase 9 (#69): the goal decides, not the model.
#
# All three cases end with the model emitting the same `finish`. What separates
# them is the machine each one leaves behind. If the objective check cannot
# overrule a clean termination, it is decoration.
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}
OUT=build/goal-eval.jsonl

# Two of the three cases are SUPPOSED to fail, so the suite exits non-zero.
# That is the result under test, not a problem running it.
"$SPG_BIN" eval examples/eval/machine/closed_loop_goal.spg >"$OUT" || true

python3 - "$OUT" <<'PY'
import json, sys

rows = [json.loads(l) for l in open(sys.argv[1])]
by = {r["name"]: r for r in rows[:-1]}
summary = rows[-1]

hot = by["false_finish_while_hot"]
# The run ended cleanly. That is exactly what makes this the case worth having.
assert hot["termination"] == "finished", hot
assert hot["outcome"] != "pass", hot
assert hot["goal"] == "temperature_too_high", hot

met = by["goal_met"]
assert met["outcome"] == "pass", met
assert met["goal"] == "satisfied", met

# The same script and the same words from the model — only the machine differs.
assert hot["steps"] == met["steps"], (hot, met)

acted = by["acted_when_forbidden"]
assert acted["termination"] == "finished", acted
assert acted["goal"] == "too_many_actions", acted
assert acted["outcome"] != "pass", acted
# Actions, not steps: the bound is on what the run DID.
assert acted["actions"] == 1, acted

assert summary["passed"] == 1, summary
PY

echo "test_cli_goal: PASS"
