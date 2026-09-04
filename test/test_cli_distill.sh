#!/bin/sh
# #26: the skill mint gate and shape-triggered injection, end to end.
# A skill is the closest thing this runtime has to a self-created rule; the
# acceptance gate is the difference between a rule the world proved and a
# model writing its own permission slip.
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

fail() {
    echo "test_cli_distill: FAIL — $1" >&2
    exit 1
}

# A policy granting exactly ONE capability, so the policy-derived shape equals
# the trajectory shape of the run below (local_shell:build.run).
cat > "$T/policy.spg" <<'EOF'
(policy
 (network_default deny)
 (budgets
  (inference_steps 8)
  (tokens 256)
  (shell_actions 2)
  (sim_actions 0)
  (wall_ms 10000))
 (capability
  ((name build.run) (kind local_shell) (enabled true) (budget 2))))
EOF

make_run() { # $1 journal, $2 expect
    cat > "$T/run.spg" <<EOF
(run
 (model "fake.gguf")
 (policy "$T/policy.spg")
 (scenario "examples/scenario.spg")
 (corpus "examples/corpus.spg")
 (journal "$1")
 (seed 42)
 (budgets (inference_steps 8) (tokens 256) (shell_actions 2) (sim_actions 0) (wall_ms 10000))
 (expect "$2"))
EOF
}

cat > "$T/script.txt" <<'EOF'
(recommend (kind local_shell) (capability "build.run") (cost 1) (uses_network false) (confidence_bp 7000) (reason "task") (command "echo SKILL-PROOF"))
(recommend (kind finish) (reason "done"))
EOF

SUITE=examples/eval/suite.spg

# --- a PASSING run, and the loop itself writes no skill ---------------------
make_run "$T/pass.sgj" "SKILL-PROOF"
"$SPG_BIN" agent --config "$T/run.spg" --fake-script "$T/script.txt" \
    --allow-exec --max-steps 4 --memory-dir "$T/mem" > "$T/pass.out" ||
    fail "the passing run failed"
grep -q "verdict=pass" "$T/pass.out" || fail "expected a pass verdict"
if ls "$T/mem" 2>/dev/null | grep -q '^skill-'; then
    fail "the agent loop minted a skill at runtime — minting is offline only"
fi

# --- a FAILING run is refused: non-zero exit, no memory write ---------------
make_run "$T/fail.sgj" "NEVER-THERE"
"$SPG_BIN" agent --config "$T/run.spg" --fake-script "$T/script.txt" \
    --allow-exec --max-steps 4 --memory-dir "$T/mem" > /dev/null 2>&1 || true
if "$SPG_BIN" distill "$T/fail.sgj" --suite "$SUITE" --memory-dir "$T/mem" \
    > "$T/dfail.out" 2>&1; then
    fail "a failed trajectory minted a skill"
fi
if ls "$T/mem" 2>/dev/null | grep -q '^skill-'; then
    fail "a refused mint still wrote memory"
fi

# --- no terminal verdict at all: refused with its OWN status ----------------
make_run "$T/noverdict.sgj" "x"
sed -i.bak '/expect/d' "$T/run.spg"
"$SPG_BIN" agent --config "$T/run.spg" --fake-script "$T/script.txt" \
    --allow-exec --max-steps 4 --memory-dir "$T/mem" > /dev/null 2>&1 || true
rc=0
"$SPG_BIN" distill "$T/noverdict.sgj" --suite "$SUITE" --memory-dir "$T/mem" \
    > "$T/dnov.out" 2>&1 || rc=$?
[ "$rc" -eq 3 ] || fail "an unjudged journal must be refused with its own status (got $rc)"
grep -q "no terminal verdict" "$T/dnov.out" ||
    fail "the unjudged refusal does not say why"

# --- the gate is not optional ----------------------------------------------
if "$SPG_BIN" distill "$T/pass.sgj" --memory-dir "$T/mem" >/dev/null 2>&1; then
    fail "distill ran without --suite — the gate must not be skippable"
fi

# --- a passing journal mints THROUGH the gate ------------------------------
"$SPG_BIN" distill "$T/pass.sgj" --suite "$SUITE" --memory-dir "$T/mem" \
    > "$T/dpass.out" || fail "gated mint failed"
grep -q '"skill":"skill-local-shell-build-run"' "$T/dpass.out" ||
    fail "unexpected skill slug: $(cat "$T/dpass.out")"
grep -q '"kept":true' "$T/dpass.out" || fail "the harmless skill was not kept"
test -f "$T/mem/skill-local-shell-build-run.md" || fail "skill file missing"

# --- same shape twice: updated in place, the store does not grow ------------
"$SPG_BIN" distill "$T/pass.sgj" --suite "$SUITE" --memory-dir "$T/mem" \
    > /dev/null || fail "re-mint failed"
[ "$(ls "$T/mem" | grep -c '^skill-')" = "1" ] ||
    fail "the dedup-by-slug invariant broke"

# --- B: the skill is injected into a later SAME-shape run as ONE directive --
make_run "$T/later.sgj" "SKILL-PROOF"
"$SPG_BIN" agent --config "$T/run.spg" --fake-script "$T/script.txt" \
    --allow-exec --max-steps 4 --memory-dir "$T/mem" > /dev/null ||
    fail "the later same-shape run failed"
strings "$T/later.sgj" | grep -q '(directive "' ||
    fail "the skill was not injected as a directive"

# --- and the off-switch: byte-identical to a run without the feature --------
make_run "$T/off1.sgj" "SKILL-PROOF"
"$SPG_BIN" agent --config "$T/run.spg" --fake-script "$T/script.txt" \
    --allow-exec --max-steps 4 --memory-dir "$T/mem" --no-skill-inject \
    > /dev/null || fail "off-switch run failed"
if strings "$T/off1.sgj" | grep -q '(directive "'; then
    fail "--no-skill-inject still injected"
fi

# --- shape mismatch: a run of a DIFFERENT shape sees no directive -----------
cat > "$T/policy2.spg" <<'EOF'
(policy
 (network_default deny)
 (budgets
  (inference_steps 8)
  (tokens 256)
  (shell_actions 0)
  (sim_actions 8)
  (wall_ms 10000))
 (capability
  ((name sim.act) (kind simulator) (enabled true) (budget 8))))
EOF
cat > "$T/run2.spg" <<EOF
(run
 (model "fake.gguf")
 (policy "$T/policy2.spg")
 (scenario "examples/scenario.spg")
 (corpus "examples/corpus.spg")
 (journal "$T/other.sgj")
 (seed 42)
 (budgets (inference_steps 8) (tokens 256) (shell_actions 0) (sim_actions 8) (wall_ms 10000)))
EOF
printf '(recommend (kind simulator) (capability "sim.act") (cost 1) (uses_network false) (confidence_bp 7000) (reason "sim"))\n(recommend (kind finish) (reason "done"))\n' > "$T/script2.txt"
"$SPG_BIN" agent --config "$T/run2.spg" --fake-script "$T/script2.txt" \
    --max-steps 4 --memory-dir "$T/mem" > /dev/null ||
    fail "the other-shape run failed"
if strings "$T/other.sgj" | grep -q '(directive "'; then
    fail "a skill for shape X was injected into a run of shape Y"
fi

# --- the gate DELETES a regressing skill and reports it ---------------------
# The suite case expects a REJECTED termination behind a gate marker that
# matches the tentative skill's index entry: with the skill present the gate
# opens, the case finishes, the expectation breaks — a regression the mint
# must revert, leaving the store byte-identical to before.
cat > "$T/gated.spg" <<EOF
(eval_suite
 (config "examples/run.spg")
 (case (name "breaks-under-skill") (script "examples/eval/sim_finish.txt")
       (max_steps 3) (gate_marker "skill-local-shell-build-run")
       (expect (termination rejected))))
EOF
"$SPG_BIN" distill "$T/pass.sgj" --suite "$T/gated.spg" \
    --memory-dir "$T/mem2" > "$T/dreject.out" || fail "regression mint errored"
grep -q '"kept":false' "$T/dreject.out" ||
    fail "a regressing skill was kept: $(cat "$T/dreject.out")"
if ls "$T/mem2" 2>/dev/null | grep -q '^skill-'; then
    fail "the reverted skill left a file behind"
fi

echo "test_cli_distill: PASS"
