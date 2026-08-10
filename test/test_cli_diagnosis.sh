#!/bin/sh
# Roadmap phase 4 (#64): the diagnosis harness, checked in both directions.
#
# The ground-truth suite passing 9/9 proves nothing on its own — it would pass
# just as happily if the metrics were hard-wired to "correct". So the negative
# suite runs the SAME states with deliberately wrong answers, and this test
# fails unless the harness catches each one.
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}

"$SPG_BIN" eval examples/eval/machine/diagnosis.spg >build/diagnosis.jsonl
"$SPG_BIN" eval examples/eval/machine/diagnosis_negative.spg \
    >build/diagnosis-negative.jsonl || true

python3 - build/diagnosis.jsonl build/diagnosis-negative.jsonl <<'PY'
import json, sys

def load(path):
    rows = [json.loads(l) for l in open(path)]
    return rows[:-1], rows[-1]

cases, summary = load(sys.argv[1])
assert len(cases) == 9, cases
assert summary["passed"] == 9, summary
d = summary["diagnosis"]
# Known and held-out are counted apart: an average would let a suite look
# strong while having learnt only the cases it was shown.
assert d["known"] == 6 and d["known_runs"] == 6, d
assert d["heldout"] == 3 and d["heldout_runs"] == 3, d
assert d["hallucinated"] == 0 and d["action_proposed"] == 0, d
for c in cases:
    assert c["emitted"] == c["expected"], c
    assert c["context_bytes"] > 0, c

neg, nsummary = load(sys.argv[2])
by = {c["name"]: c for c in neg}
# a wrong root cause must fail the case, not merely be noted
assert by["wrong_category"]["correct"] == 0, by["wrong_category"]
assert by["wrong_category"]["outcome"] == "fail_observation", by["wrong_category"]
# an invented process is caught even when the category is right
assert by["hallucinated_process"]["hallucinated"] == 1, by["hallucinated_process"]
# acting when only a diagnosis was asked for is counted
assert by["action_proposed"]["action_proposed"] == 1, by["action_proposed"]
# Both of these are verbatim Gemma answers. Neither is a hallucination and
# neither is a missing diagnosis — the metric must not measure punctuation.
assert by["bracketed_ids"]["hallucinated"] == 0, by["bracketed_ids"]
assert by["bracketed_ids"]["correct"] == 1, by["bracketed_ids"]
assert by["punctuated_category"]["emitted"] == "healthy", by["punctuated_category"]
# A model-supplied reason is the one field an outside party writes into this
# report, and BitNet-large put a newline in one. Emitted raw it truncated the
# JSONL line mid-string, and the tooling read the unparseable line as "this
# model produced no scoreable run" — a measurement bug wearing a model result's
# clothes. The guarantee is now unconditional: every line parses, whatever the
# model wrote. The json.loads above IS that assertion, on every line.
#
# The case itself is rejected, and that is correct: a literal newline inside an
# s-expression string is not valid input. The escaping matters for the RAW
# output that reaches the report after such a rejection.
assert by["reason_with_newline"]["parsed"] == 0, by["reason_with_newline"]
assert by["reason_with_newline"]["termination"] == "rejected", \
    by["reason_with_newline"]
assert nsummary["passed"] == 4, nsummary
assert nsummary["total"] == 6, nsummary
PY

echo "test_cli_diagnosis: PASS"
