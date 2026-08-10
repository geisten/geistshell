#!/bin/sh
# Roadmap phase 6 (#66): does a pause actually stop a process, and does the
# identity check actually stop a signal?
#
# Everything else about this phase is checked in test_machine_action.c against
# fixtures. This is the part fixtures cannot answer: whether the syscall lands
# on the right process. Linux only — elsewhere the executor refuses to act
# because it cannot re-read /proc to confirm identity, and that refusal is
# itself what gets checked.
set -eu

BIN=${SPG_MACHINE_PROBE:-${TEST_DIR:-build/host-debug/test}/machine_action_probe}

if [ ! -x "$BIN" ]; then
    echo "test_cli_machine_action: SKIP (probe not built)"
    exit 0
fi

"$BIN"
echo "test_cli_machine_action: PASS"
