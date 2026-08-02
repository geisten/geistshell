#!/bin/sh
# System test: the dedicated memory_actions budget limits governed memory
# actions. With memory_actions=1, the second memory_save in a 2-tick run is
# denied by the global budget; consumed.memory_actions reflects the one allowed.
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

cat > "$T/policy.spg" <<'EOF'
(policy (network_default deny)
 (budgets (inference_steps 8) (tokens 256) (shell_actions 1) (sim_actions 8) (memory_actions 1) (wall_ms 10000) (journal_bytes 1048576) (risk_bp 10000))
 (capability ((name mem.write) (kind memory) (enabled true) (budget 8))))
EOF
cat > "$T/run.spg" <<EOF
(run (model "fake.gguf") (policy "$T/policy.spg") (scenario "examples/scenario.spg") (corpus "examples/corpus.spg") (journal "$T/j.sgj") (seed 42)
 (budgets (inference_steps 8) (tokens 256) (shell_actions 1) (sim_actions 8) (wall_ms 10000) (journal_bytes 1048576) (risk_bp 10000)))
EOF
REC='(recommend (kind memory_save) (capability "mem.write") (cost 1) (uses_network false) (confidence_bp 9000) (reason "save") (slug "k") (description "d") (body "b"))'

"$SPG_BIN" run --config "$T/run.spg" --memory-dir "$T/mem" --ticks 2 --fake "$REC" \
    --write-run-state "$T/state.json" >/dev/null 2>&1

# exactly one memory action was allowed/consumed
grep -q '"memory_actions":1' "$T/state.json"

# the second tick was denied on the budget
"$SPG_BIN" replay "$T/j.sgj" | grep -q '"decision":"deny"'
DENY=$("$SPG_BIN" replay "$T/j.sgj" | grep -c '"decision":"deny"')
test "$DENY" = "1"

# a policy that omits memory_actions still allows memory (defaults to unlimited)
cat > "$T/policy2.spg" <<'EOF'
(policy (network_default deny)
 (budgets (inference_steps 8) (tokens 256) (shell_actions 1) (sim_actions 8) (wall_ms 10000) (journal_bytes 1048576) (risk_bp 10000))
 (capability ((name mem.write) (kind memory) (enabled true) (budget 8))))
EOF
cat > "$T/run2.spg" <<EOF
(run (model "fake.gguf") (policy "$T/policy2.spg") (scenario "examples/scenario.spg") (corpus "examples/corpus.spg") (journal "$T/j2.sgj") (seed 42)
 (budgets (inference_steps 8) (tokens 256) (shell_actions 1) (sim_actions 8) (wall_ms 10000) (journal_bytes 1048576) (risk_bp 10000)))
EOF
"$SPG_BIN" run --config "$T/run2.spg" --memory-dir "$T/mem2" --ticks 2 --fake "$REC" >/dev/null 2>&1
test "$("$SPG_BIN" replay "$T/j2.sgj" | grep -c '"decision":"deny"')" = "0"

echo "test_cli_memory_budget: PASS"
