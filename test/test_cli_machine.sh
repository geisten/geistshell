#!/bin/sh
# Roadmap phase 3: the machine snapshot reaches the model's context, and only
# when asked for.
#
# The default path must stay byte-identical to before this phase — that is what
# keeps the phase-0 journal freeze meaningful — so the same run is checked twice,
# once with --machine and once without.
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}
JOURNAL=build/tick-demo.sgj
SCRIPT=build/test-cli-machine-fake.txt

printf '(recommend (kind finish) (reason "observed"))\n' >"$SCRIPT"

run() {
    rm -f "$JOURNAL"
    # shellcheck disable=SC2086
    "$SPG_BIN" agent --config examples/run.spg --fake-script "$SCRIPT" \
        --max-steps 1 $1 >/dev/null
}

run "--machine"
BLOCK=$(strings "$JOURNAL" | grep -o '(machine-state .*' | head -1 || true)
if [ -z "$BLOCK" ]; then
    echo "test_cli_machine: FAIL — no (machine-state ...) in the model input" >&2
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

run ""
if strings "$JOURNAL" | grep -q "machine-state"; then
    echo "test_cli_machine: FAIL — machine state leaked into the default run" >&2
    exit 1
fi

echo "test_cli_machine: PASS"
