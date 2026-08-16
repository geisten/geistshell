#!/bin/sh
# System test: governed memory_read (recall surfaces into the next tick's
# context) and memory_delete (removes the file), through the run pipeline.
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

cat > "$T/policy.spg" <<'EOF'
(policy (network_default deny)
 (budgets (inference_steps 8) (tokens 256) (shell_actions 1) (sim_actions 8) (wall_ms 10000))
 (capability ((name mem.write) (kind memory) (enabled true) (budget 8))))
EOF
cat > "$T/run.spg" <<EOF
(run (model "fake.gguf") (policy "$T/policy.spg") (scenario "examples/scenario.spg") (corpus "examples/corpus.spg") (journal "$T/j.sgj") (seed 42)
 (budgets (inference_steps 8) (tokens 256) (shell_actions 1) (sim_actions 8) (wall_ms 10000)))
EOF

printf 'REMEMBERED SECRET\n' | "$SPG_BIN" memory --dir "$T/mem" save secret "a secret hook" >/dev/null

# --- READ: two ticks; the recalled content lands in the later tick's context ---
"$SPG_BIN" run --config "$T/run.spg" --memory-dir "$T/mem" --ticks 2 \
    --fake '(recommend (kind memory_read) (capability "mem.write") (cost 1) (uses_network false) (confidence_bp 9000) (reason "recall") (slug "secret"))' \
    >/dev/null 2>&1
# the recalled content is rendered into the next tick's context via the
# generic observation channel (journaled as model input)
grep -aq "(observation " "$T/j.sgj"
grep -aq "REMEMBERED SECRET" "$T/j.sgj"
"$SPG_BIN" verify-journal "$T/j.sgj" | grep -q "event.memory"

# --- DELETE: removes the file ---
test -f "$T/mem/secret.md"
rm "$T/j.sgj"
"$SPG_BIN" run --config "$T/run.spg" --memory-dir "$T/mem" --ticks 1 \
    --fake '(recommend (kind memory_delete) (capability "mem.write") (cost 1) (uses_network false) (confidence_bp 9000) (reason "forget") (slug "secret"))' \
    >/dev/null 2>&1
if [ -f "$T/mem/secret.md" ]; then
    echo "memory_delete did not remove the file" >&2
    exit 1
fi

echo "test_cli_memory_recall: PASS"
