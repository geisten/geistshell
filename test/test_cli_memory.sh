#!/bin/sh
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}
DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT

printf 'body one\n' | "$SPG_BIN" memory --dir "$DIR" save note-one "first hook" >/dev/null
printf 'body two\n' | "$SPG_BIN" memory --dir "$DIR" save note-two "second hook" >/dev/null

LIST=$("$SPG_BIN" memory --dir "$DIR" list)
echo "$LIST" | grep -q "note-one: first hook"
echo "$LIST" | grep -q "note-two: second hook"

READ=$("$SPG_BIN" memory --dir "$DIR" read note-one)
echo "$READ" | grep -q "name: note-one"
echo "$READ" | grep -q "body one"

"$SPG_BIN" memory --dir "$DIR" delete note-one >/dev/null
LIST2=$("$SPG_BIN" memory --dir "$DIR" list)
if echo "$LIST2" | grep -q "note-one"; then
    echo "delete did not remove note-one" >&2
    exit 1
fi
echo "$LIST2" | grep -q "note-two: second hook"

# Path-traversal slug must be rejected (non-zero exit, nothing written).
if printf x | "$SPG_BIN" memory --dir "$DIR" save ../evil bad >/dev/null 2>&1; then
    echo "traversal slug was not rejected" >&2
    exit 1
fi

echo "test_cli_memory: PASS"
