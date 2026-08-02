#!/bin/sh
# System test: the governed memory_save action through the run pipeline — policy
# allows it (file written + journaled + deterministic), and a disabled policy
# capability denies it (nothing written).
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

REC='(recommend (kind memory_save) (capability "mem.write") (cost 1) (uses_network false) (confidence_bp 9000) (reason "save a fact") (slug "fact1") (description "a saved fact") (body "the body text"))'

write_run_config() { # $1 = policy path
    cat > "$T/run.spg" <<EOF
(run (model "fake.gguf") (policy "$1") (scenario "examples/scenario.spg") (corpus "examples/corpus.spg") (journal "$T/j.sgj") (seed 42)
 (budgets (inference_steps 8) (tokens 256) (shell_actions 1) (sim_actions 8) (wall_ms 10000) (journal_bytes 1048576) (risk_bp 10000)))
EOF
}

# --- ALLOW: capability enabled -> the action persists and is journaled ---
cat > "$T/policy-allow.spg" <<'EOF'
(policy (network_default deny)
 (budgets (inference_steps 8) (tokens 256) (shell_actions 1) (sim_actions 8) (wall_ms 10000) (journal_bytes 1048576) (risk_bp 10000))
 (capability ((name mem.write) (kind memory) (enabled true) (budget 8))))
EOF
write_run_config "$T/policy-allow.spg"

"$SPG_BIN" run --config "$T/run.spg" --memory-dir "$T/mem" --fake "$REC" --ticks 1 >/dev/null 2>&1
test -f "$T/mem/fact1.md"
grep -q "name: fact1" "$T/mem/fact1.md"
grep -q "the body text" "$T/mem/fact1.md"
"$SPG_BIN" verify-journal "$T/j.sgj" | grep -q "event.memory"

# determinism: a second run from the SAME starting state yields a byte-identical
# journal. The memory store persists across runs (that is the point), so it must
# be reset to its initial empty state for the comparison to hold.
cp "$T/j.sgj" "$T/j_first.sgj"
rm -rf "$T/j.sgj" "$T/mem"
"$SPG_BIN" run --config "$T/run.spg" --memory-dir "$T/mem" --fake "$REC" --ticks 1 >/dev/null 2>&1
cmp "$T/j_first.sgj" "$T/j.sgj"

# --- DENY: capability disabled -> policy gate blocks it, nothing written ---
cat > "$T/policy-deny.spg" <<'EOF'
(policy (network_default deny)
 (budgets (inference_steps 8) (tokens 256) (shell_actions 1) (sim_actions 8) (wall_ms 10000) (journal_bytes 1048576) (risk_bp 10000))
 (capability ((name mem.write) (kind memory) (enabled false) (budget 8))))
EOF
write_run_config "$T/policy-deny.spg"
rm -rf "$T/mem2" "$T/j.sgj"
"$SPG_BIN" run --config "$T/run.spg" --memory-dir "$T/mem2" --fake "$REC" --ticks 1 >/dev/null 2>&1
if [ -f "$T/mem2/fact1.md" ]; then
    echo "denied memory action still wrote a file" >&2
    exit 1
fi
"$SPG_BIN" replay "$T/j.sgj" | grep -q '"decision":"deny"'

echo "test_cli_memory_action: PASS"
