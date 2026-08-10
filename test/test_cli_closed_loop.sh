#!/bin/sh
# Roadmap phase 7 (#67): observe -> act -> observe -> reassess.
#
# The eval suite checks that each case terminates the way it should. That is
# not enough: a run can end "finished" while the agent never saw the effect of
# its own action. What makes this a loop rather than a sequence of decisions is
# that the SECOND tick's context carries the world the FIRST tick changed, and
# the only place that can be verified is the journal.
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}
JOURNAL=build/machine-demo.sgj

rm -f "$JOURNAL" "$JOURNAL.lock"
"$SPG_BIN" eval examples/eval/machine/closed_loop_pause.spg >build/closed-loop.jsonl

python3 - build/closed-loop.jsonl <<'PY'
import json, sys

rows = [json.loads(l) for l in open(sys.argv[1])]
cases, summary = {c["name"]: c for c in rows[:-1]}, rows[-1]
assert summary["passed"] == summary["total"], summary

# The loop ran more than one tick and then stopped on its own.
assert cases["pause_then_finish"]["termination"] == "finished"
assert cases["pause_then_finish"]["steps"] == 2, cases["pause_then_finish"]

# Denied stays denied — the run does not fall through to something else.
assert cases["denied_critical_pause"]["termination"] == "denied"
# THE bypass: no local_shell capability exists in machine-policy.spg, so the
# refusal is configuration, not prompt wording.
assert cases["shell_bypass_refused"]["termination"] == "denied"
assert cases["shell_bypass_refused"]["gated"] == 0, cases["shell_bypass_refused"]

# A model that only ever acts stops at the bound instead of running forever.
assert cases["max_steps"]["termination"] == "max_steps"
PY

# The other half — that the SECOND tick's context actually carries the world
# the first tick changed — is checked in test_machine_loop.c, where the context
# buffer can be read directly after the run. The eval harness writes no
# journal, so it cannot show that from here.

echo "test_cli_closed_loop: PASS"
