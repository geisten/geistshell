#!/bin/sh
# System test: the governed multi-step agent loop. A scripted fake model drives
# local_shell + finish through policy-gating, journaling, and observation
# feedback; the loop terminates on `finish` and is run-to-run deterministic.
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
 (budgets (inference_steps 8) (tokens 256) (shell_actions 1) (sim_actions 8) (wall_ms 10000)))
EOF

# Script: one governed shell step, then finish.
cat > "$T/script.txt" <<'EOF'
(recommend (kind local_shell) (capability "build.run") (cost 1) (uses_network false) (confidence_bp 6000) (reason "probe") (command "echo AGENT-LOOP-PROOF"))
(recommend (kind finish) (reason "task complete"))
EOF

# --- governed run with exec enabled: loop runs the shell, observes, finishes ---
"$SPG_BIN" agent --config "$T/run.spg" --fake-script "$T/script.txt" \
    --allow-exec --max-steps 5 > "$T/agent.out" 2>&1
grep -q "termination=finished" "$T/agent.out"
grep -q "steps=2" "$T/agent.out"
# The shell stdout was fed back into the observation channel.
grep -q "AGENT-LOOP-PROOF" "$T/agent.out"

# --- the journal records the governed steps (policy decision + action) ---
V=$("$SPG_BIN" verify-journal "$T/j.sgj")
echo "$V" | grep -q "verified=true"
echo "$V" | grep -q "status_failures=0"
echo "$V" | grep -q "event.action=1"

# --- two runs are deterministic in everything but perception ---
# The model input carries a live (machine-state ...) block, so its bytes vary
# run to run — that is measurement, not nondeterminism. Everything else
# (sequence, parents, logical timestamps, actions, outcomes) must be
# byte-identical; a wall clock or an unstable ordering would show here.
"$SPG_BIN" replay "$T/j.sgj" | grep -v '"event":"model_input"' \
    > "$T/timeline_first.jsonl"
rm "$T/j.sgj"
"$SPG_BIN" agent --config "$T/run.spg" --fake-script "$T/script.txt" \
    --allow-exec --max-steps 5 >/dev/null 2>&1
"$SPG_BIN" replay "$T/j.sgj" | grep -v '"event":"model_input"' \
    > "$T/timeline_second.jsonl"
cmp "$T/timeline_first.jsonl" "$T/timeline_second.jsonl"

# --- without --allow-exec the boundary denies, but the loop still finishes ---
rm "$T/j.sgj"
"$SPG_BIN" agent --config "$T/run.spg" --fake-script "$T/script.txt" \
    --max-steps 5 > "$T/agent_denied.out" 2>&1
grep -q "termination=finished" "$T/agent_denied.out"
grep -q "denied" "$T/agent_denied.out"

# --- self-repair: a malformed first reply is repaired, then finish ---
rm "$T/j.sgj"
cat > "$T/bad_script.txt" <<'EOF'
this is not a recommendation
(recommend (kind finish) (reason "task complete"))
EOF
"$SPG_BIN" agent --config "$T/run.spg" --fake-script "$T/bad_script.txt" \
    --max-steps 5 --max-repairs 2 > "$T/agent_repair.out" 2>&1
grep -q "termination=finished" "$T/agent_repair.out"
grep -q "steps=2" "$T/agent_repair.out"

# --- with no repair budget the same script terminates rejected ---
rm "$T/j.sgj"
"$SPG_BIN" agent --config "$T/run.spg" --fake-script "$T/bad_script.txt" \
    --max-steps 5 --max-repairs 0 > "$T/agent_norepair.out" 2>&1
grep -q "termination=rejected" "$T/agent_norepair.out"

# --- the policy step budget (inference_steps) caps the loop before finish ---
cat > "$T/run1.spg" <<EOF
(run
 (model "fake.gguf")
 (policy "examples/policy.spg")
 (scenario "examples/scenario.spg")
 (corpus "examples/corpus.spg")
 (journal "$T/j1.sgj")
 (seed 42)
 (budgets (inference_steps 1) (tokens 256) (shell_actions 1) (sim_actions 8) (wall_ms 10000)))
EOF
cat > "$T/sim_script.txt" <<'EOF'
(recommend (kind simulator) (capability "sim.act") (cost 1) (uses_network false) (confidence_bp 7000) (reason "r"))
(recommend (kind finish) (reason "done"))
EOF
"$SPG_BIN" agent --config "$T/run1.spg" --fake-script "$T/sim_script.txt" \
    --max-steps 10 > "$T/agent_budget.out" 2>&1
grep -q "termination=budget" "$T/agent_budget.out"
grep -q "steps=1" "$T/agent_budget.out"

echo "test_cli_agent: PASS"
