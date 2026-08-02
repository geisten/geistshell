#!/bin/sh
# System test: the chat REPL in offline (--fake) mode — transcript output and
# memory slash-commands — without needing a real model.
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}
CHAT="$(dirname "$SPG_BIN")/geistshell-chat"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

# --- transcript records both roles ---
printf 'hello there\n/quit\n' | "$CHAT" --fake --transcript "$T/t.jsonl" >/dev/null 2>&1
grep -q '"role":"user","content":"hello there"' "$T/t.jsonl"
grep -q '"role":"assistant"' "$T/t.jsonl"

# --- /reset and unknown-ish memory commands are handled, not sent to a model ---
OUT=$(printf '/memories\n/reset\n/quit\n' | "$CHAT" --fake 2>&1)
echo "$OUT" | grep -q "memory unavailable"
echo "$OUT" | grep -q "context cleared"

# --- a bad flag is a usage error (exit 2) ---
set +e
printf '/quit\n' | "$CHAT" --bogus-flag >/dev/null 2>&1
rc=$?
set -e
test "$rc" = "2"

echo "test_cli_chat: PASS"
