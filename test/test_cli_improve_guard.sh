#!/bin/sh
# System test: improve --guard wiring (docs/LEARNING.md P5, Weg 2). The full
# veto needs a real model reacting to the mind-palace lesson (a fake model
# ignores it), so that path is exercised on hardware. Here we assert the
# wiring is safe WITHOUT a model: a guard whose (model "geist") cannot load
# fails at baseline, which spg_guard_survives treats as no signal, so guards
# degrade to inert and the suite gate alone decides — identical to no --guard.
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

# A guard run config with a criterion (expect). Its model is a fake path that
# won't load as a real (geist) model in this environment.
cat > "$T/guard.spg" <<EOF
(run
 (model "fake.gguf")
 (policy "examples/policy.spg")
 (scenario "examples/scenario.spg")
 (corpus "examples/corpus.spg")
 (journal "$T/g.sgj")
 (seed 42)
 (budgets (inference_steps 8) (tokens 256) (shell_actions 1) (sim_actions 8) (wall_ms 10000))
 (expect "PROOF"))
EOF

MEM=$(mktemp -d)

# improve over the shipped gated suite WITH a guard: the guard is inert here
# (no real model), so the run completes and the suite gate decides exactly as
# it would without --guard.
OUT_GUARD=$("$SPG_BIN" improve examples/eval/improve_gated.spg \
    --memory-dir "$MEM" --guard "$T/guard.spg" 2>&1)
echo "$OUT_GUARD" | grep -q '"lessons_kept"'

# the same run without --guard: the final summary line must match (guards
# contributed no signal, so the decision is unchanged)
MEM2=$(mktemp -d)
OUT_PLAIN=$("$SPG_BIN" improve examples/eval/improve_gated.spg \
    --memory-dir "$MEM2" 2>&1)

KEPT_G=$(printf '%s\n' "$OUT_GUARD" | sed -n 's/.*"lessons_kept":\([0-9]*\).*/\1/p' | tail -1)
KEPT_P=$(printf '%s\n' "$OUT_PLAIN" | sed -n 's/.*"lessons_kept":\([0-9]*\).*/\1/p' | tail -1)
[ "$KEPT_G" = "$KEPT_P" ]

rm -rf "$MEM" "$MEM2"
echo "test_cli_improve_guard: PASS"
