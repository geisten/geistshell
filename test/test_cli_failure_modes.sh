#!/bin/sh
# Roadmap phase 12 (#72): the failure categorisation is a rule, not a reading.
#
# Every synthetic case below exhibits exactly one failure mode, and the
# categoriser must find that one. A distribution built by hand drifts with
# whoever is looking at it, and the decision on fine-tuning rests on that
# distribution.
set -eu

T=build/failure-modes
rm -rf "$T"
mkdir -p "$T"

# Hand-written records rather than model runs: the categoriser is being tested,
# not a model, and a rule needs inputs that isolate one symptom each.
cat >"$T/cases.jsonl" <<'EOF'
{"name":"ok","outcome":"pass","parsed":1,"correct":1,"steps":1,"goal":"satisfied"}
{"name":"unparseable","outcome":"fail_termination","parsed":0,"correct":0,"steps":1}
{"name":"acted_on_a_misreading","outcome":"fail_observation","parsed":1,"correct":0,"action_proposed":1,"steps":2}
{"name":"acted_though_right","outcome":"fail_observation","parsed":1,"correct":1,"action_proposed":1,"steps":2}
{"name":"left_it_too_hot","outcome":"fail_observation","parsed":1,"correct":1,"steps":1,"goal":"temperature_too_high"}
{"name":"cast_about","outcome":"fail_steps","parsed":1,"correct":1,"steps":4,"goal":"satisfied"}
{"name":"named_the_wrong_cause","outcome":"fail_observation","parsed":1,"correct":0,"steps":1,"goal":"satisfied"}
EOF

python3 - "$T/cases.jsonl" <<'PY'
import json, subprocess, sys

out = subprocess.run(
    ["python3", "examples/machine/categorise_failures.py", sys.argv[1]],
    capture_output=True, text=True, check=True)
r = json.loads(out.stdout)

assert r["total_runs"] == 7, r
assert r["failed_runs"] == 6, r
assert r["modes"] == {
    "parse_failure": 1,
    "unsafe_action": 1,
    "right_diagnosis_wrong_action": 1,
    "constraint_violation": 1,
    "excessive_steps": 1,
    "wrong_diagnosis": 1,
}, r["modes"]

# Each mode carries a verbatim example: a distribution nobody can trace back to
# a run is a distribution nobody can argue with.
for mode, ex in r["examples"].items():
    assert ex["case"], (mode, ex)

# The mode that CANNOT be measured is named rather than reported as zero —
# there is no history in the context yet, so no run can fail by not using it.
assert "inability_to_use_temporal_history" in r["unmeasured"], r
PY

echo "test_cli_failure_modes: PASS"
