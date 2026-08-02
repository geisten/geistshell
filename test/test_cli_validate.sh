#!/bin/sh
# System test: the validation subcommands (policy-check, sim-validate) on valid
# inputs and on malformed input (exit code + summary).
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

# --- valid policy ---
P=$("$SPG_BIN" policy-check examples/policy.spg)
echo "$P" | grep -q "valid=true"
echo "$P" | grep -q "network_default=deny"
echo "$P" | grep -q "capabilities=3"

# --- malformed policy is rejected (non-zero exit) ---
printf '(policy (network_default bogus (budgets' > "$T/bad-policy.spg"
if "$SPG_BIN" policy-check "$T/bad-policy.spg" >/dev/null 2>&1; then
    echo "malformed policy was accepted" >&2
    exit 1
fi

# --- valid scenario ---
S=$("$SPG_BIN" sim-validate examples/scenario.spg)
echo "$S" | grep -q "valid=true"
echo "$S" | grep -q "hosts=2"

# --- malformed scenario is rejected ---
printf '(scenario (host (id' > "$T/bad-scenario.spg"
if "$SPG_BIN" sim-validate "$T/bad-scenario.spg" >/dev/null 2>&1; then
    echo "malformed scenario was accepted" >&2
    exit 1
fi

# --- missing file is rejected, not a crash ---
if "$SPG_BIN" policy-check "$T/does-not-exist.spg" >/dev/null 2>&1; then
    echo "missing policy file was accepted" >&2
    exit 1
fi

echo "test_cli_validate: PASS"
