#!/usr/bin/env python3
"""Assign every failed diagnosis run to exactly ONE failure mode (roadmap
phase 12, #72).

A rule, not a judgement call. Categories added by hand after the fact drift
with whoever is reading, and the whole point of the distribution is to show
whether one failure mode recurs often enough to be worth training against.

The order below IS the priority. A run can exhibit several symptoms — an
unparseable answer that also took too many steps — and the first match wins,
because a category per failure is what makes the counts add up.
"""
import json
import sys

# Priority order. Earlier entries win; each predicate takes one case record.
RULES = [
    # Nothing else can be judged if the form never arrived.
    ("parse_failure", lambda c: not c.get("parsed")),
    # An action proposed on a diagnosis-only suite is out of scope by
    # construction. With a wrong cause behind it, it is the dangerous kind:
    # acting on a misreading.
    ("unsafe_action", lambda c: c.get("action_proposed") and not c.get("correct")),
    # Same action, but the reasoning was right — a governance problem, not a
    # perception one.
    ("right_diagnosis_wrong_action",
     lambda c: c.get("action_proposed") and c.get("correct")),
    # The machine still violates the goal, whatever the model concluded.
    ("constraint_violation",
     lambda c: c.get("goal") not in (None, "satisfied", "no_goal")),
    # One step is enough to name a cause; more means the model was casting
    # about.
    ("excessive_steps", lambda c: (c.get("steps") or 0) > 1),
    # The residual: the form was fine, nothing was touched, the cause was wrong.
    ("wrong_diagnosis", lambda c: not c.get("correct")),
]

# Named so its absence is visible in every report rather than silently missing:
# there is no history in the context yet (#79), so no run can fail by being
# unable to use it. Reporting 0 would suggest it was measured.
UNMEASURED = ["inability_to_use_temporal_history"]


def categorise(case):
    """The single failure mode of one case, or None when it passed."""
    if case.get("outcome") == "pass":
        return None
    for name, predicate in RULES:
        try:
            if predicate(case):
                return name
        except TypeError:
            continue
    return "other"


def main():
    counts = {}
    examples = {}
    total = failed = 0
    for path in sys.argv[1:]:
        with open(path, encoding="utf-8") as handle:
            for line in handle:
                line = line.strip()
                if not line:
                    continue
                record = json.loads(line)
                if "name" not in record:
                    continue
                total += 1
                mode = categorise(record)
                if mode is None:
                    continue
                failed += 1
                counts[mode] = counts.get(mode, 0) + 1
                # One verbatim example per mode: a distribution without a
                # trajectory cannot be argued with.
                examples.setdefault(mode, {
                    "case": record.get("name"),
                    "expected": record.get("expected"),
                    "emitted": record.get("emitted"),
                    "reason": record.get("reason"),
                    "steps": record.get("steps"),
                })
    print(json.dumps({
        "total_runs": total,
        "failed_runs": failed,
        "modes": dict(sorted(counts.items(), key=lambda kv: -kv[1])),
        "examples": examples,
        "unmeasured": UNMEASURED,
    }, indent=2))


if __name__ == "__main__":
    main()
