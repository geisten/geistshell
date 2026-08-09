#!/bin/sh
# Baseline freeze (Phase 0). One scripted fake run must stay byte-identical.
#
# The journal is deterministic by construction: timestamps are injected by the
# caller (src/cli/main.c: timestamp_ns = 1, 2, ...), never read from a clock,
# and they are hashed into the record chain. This test is the canary for that
# property — every later phase that touches context, actions or the journal
# must keep it.
#
# If this fails, determinism broke. Do NOT refresh the constant reflexively:
# update it only in a commit whose message names the change that legitimately
# altered the journal bytes.
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}
JOURNAL=build/tick-demo.sgj  # path comes from examples/run.spg
EXPECTED=52bd794e11885e1b8ae257ef037a1b87b2ed3fa8311ca85b557253c99e537365

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    else
        shasum -a 256 "$1" | cut -d' ' -f1
    fi
}

rm -f "$JOURNAL"
"$SPG_BIN" run \
    --config examples/run.spg \
    --fake '(recommend (kind simulator) (capability "sim.act") (cost 1) (uses_network false) (confidence_bp 9000) (reason "reduce simulated risk"))' \
    --ticks 1 \
    >/dev/null

ACTUAL=$(sha256_of "$JOURNAL")
if [ "$ACTUAL" != "$EXPECTED" ]; then
    echo "test_cli_baseline: FAIL — journal hash drifted" >&2
    echo "  expected $EXPECTED" >&2
    echo "  actual   $ACTUAL" >&2
    echo "  see docs/machine-intelligence/Baseline.md" >&2
    exit 1
fi
echo "test_cli_baseline: PASS"
