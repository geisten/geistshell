#!/bin/sh
# System test: keyed journal sealing. The hash chain already detects edits; the
# HMAC seal binds the chain head to a secret, so a holder of the key can confirm
# the log is intact AND authentic (and a re-chained forgery still fails).
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

cat > "$T/run.spg" <<EOF
(run
 (model "fake.gguf")
 (policy "examples/policy.spg")
 (scenario "examples/scenario.spg")
 (corpus "examples/corpus.spg")
 (journal "$T/j.sgj")
 (seed 42)
 (budgets (inference_steps 8) (tokens 256) (shell_actions 1) (sim_actions 8) (memory_actions 8) (wall_ms 10000) (journal_bytes 1048576) (risk_bp 10000)))
EOF
REC='(recommend (kind simulator) (capability "sim.act") (cost 1) (uses_network false) (confidence_bp 9000) (reason "reduce risk"))'
"$SPG_BIN" run --config "$T/run.spg" --fake "$REC" --ticks 2 >/dev/null 2>&1

printf 'super-secret-audit-key-001' > "$T/key"
printf 'a-different-key'            > "$T/key2"

# --- seal, then verify with the correct key: chain + seal both pass ---
"$SPG_BIN" seal-journal "$T/j.sgj" --key "$T/key" | grep -q "sealed="
test -f "$T/j.sgj.sig"
OUT=$("$SPG_BIN" verify-journal "$T/j.sgj" --key "$T/key")   # exit 0 expected
printf '%s\n' "$OUT" | grep -q "verified=true"
printf '%s\n' "$OUT" | grep -q "signed=true"

# --- sealing is deterministic: same key + journal -> identical tag ---
"$SPG_BIN" seal-journal "$T/j.sgj" --key "$T/key" --sig "$T/a.sig" >/dev/null
"$SPG_BIN" seal-journal "$T/j.sgj" --key "$T/key" --sig "$T/b.sig" >/dev/null
cmp "$T/a.sig" "$T/b.sig"

# --- wrong key: the seal does not verify, non-zero exit ---
if "$SPG_BIN" verify-journal "$T/j.sgj" --key "$T/key2" > "$T/wrong.out" 2>&1; then
    echo "expected non-zero exit for wrong key" >&2
    exit 1
fi
grep -q "signed=false" "$T/wrong.out"

# --- tampering a byte: the chain itself rejects it, non-zero exit ---
printf 'X' | dd of="$T/j.sgj" bs=1 seek=200 conv=notrunc 2>/dev/null
if "$SPG_BIN" verify-journal "$T/j.sgj" --key "$T/key" > "$T/tamper.out" 2>&1; then
    echo "expected non-zero exit for a tampered journal" >&2
    exit 1
fi

echo "test_cli_seal: PASS"
