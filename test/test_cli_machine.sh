#!/bin/sh
# Perception is automatic: every agent run carries a (machine-state ...) block
# in the model input, with no flag asked for. Only authority stays opt-in —
# managing a process needs a profile and the capability.
#
# This inverts the original phase-3 contract ("only when asked for"). The old
# byte-identity anchor was the phase-0 freeze; the new anchor is the journal
# itself: what the agent saw is journaled as MODEL_INPUT either way.
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}
JOURNAL=build/tick-demo.sgj
SCRIPT=build/test-cli-machine-fake.txt

printf '(recommend (kind finish) (reason "observed"))\n' >"$SCRIPT"

rm -f "$JOURNAL"
"$SPG_BIN" agent --config examples/run.spg --fake-script "$SCRIPT" \
    --max-steps 1 >/dev/null

BLOCK=$(strings "$JOURNAL" | grep -o '(machine-state .*' | head -1 || true)
if [ -z "$BLOCK" ]; then
    echo "test_cli_machine: FAIL — no (machine-state ...) in the default run" >&2
    exit 1
fi

# Balanced parens: a truncated block would still contain the opening token but
# could not be parsed by the model, and it would reach the journal unnoticed.
python3 - "$BLOCK" <<'PY'
import sys

block = sys.argv[1]
# Parens INSIDE a quoted string are content, not structure. The first version
# counted them all and passed for a year on Linux, where kernel comm names
# rarely contain brackets — macOS surfaced it immediately with names like
# "Claude Helper (". A checker that only works on one platform's data was never
# checking the property it claimed to.
depth, in_string, escaped = 0, False, False
for ch in block:
    if escaped:
        escaped = False
        continue
    if ch == "\\":
        escaped = True
    elif ch == '"':
        in_string = not in_string
    elif not in_string:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                break
assert not in_string, f"unterminated string in block: {block[:200]}"
assert depth == 0, f"unbalanced machine-state block: {block[:200]}"
for field in ("cpu-load-bp", "memory-total-bytes", "temperature-mc",
              "throttle", "process-count"):
    assert f"({field} " in block, f"missing field {field}"
PY

echo "test_cli_machine: PASS"
