#!/bin/sh
# System test: the guarded `exec` subcommand — capture, exit-code propagation,
# and the executor-boundary network denial — driven through the real binary.
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

# --- a known command runs and its stdout/exit are captured ---
OUT=$("$SPG_BIN" exec uname -s)
echo "$OUT" | grep -q "(started true)"
echo "$OUT" | grep -q "(exited true)"
echo "$OUT" | grep -q "(exit_code 0)"

# --- a failing command propagates its non-zero exit code ---
set +e
"$SPG_BIN" exec false >/dev/null 2>&1
rc=$?
set -e
test "$rc" = "1"

# --- a network command is blocked by the boundary (exit 3) ---
set +e
"$SPG_BIN" exec ssh -V > "$T/ssh.out" 2>&1
rc=$?
set -e
test "$rc" = "3"
grep -q "network_forbidden" "$T/ssh.out"
grep -q "(approved false)" "$T/ssh.out"

# --- no command given is a usage error ---
set +e
"$SPG_BIN" exec >/dev/null 2>&1
rc=$?
set -e
test "$rc" = "2"

echo "test_cli_exec: PASS"
