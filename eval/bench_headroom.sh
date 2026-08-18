#!/bin/sh
# Headroom census: which tasks does the guarded control arm actually fail?
#
# The directive x best-of-N measurement (LEARNING.md 2026-08-16) hit a ceiling:
# with constrained decode + the Weg-2 convergence stop, every goal-word task
# passed on the first sampled attempt, so no runtime-learning lever is decidable
# on that corpus. A lever needs tasks where control fails SOMETIMES — failures
# that depend on the model's own command choice, which no guardrail covers.
#
# This corpus is transform/compute tasks: the expected output is a function of
# the input (reverse, case, count, sum, sequence), so it cannot be produced by
# echoing the goal back; the model must pick the right shell recipe. Constrained
# decode guarantees a VALID form either way — validity is not the test, the
# decision is.
#
# Per task: one greedy run (T=0) + K independent sampled runs (T, seeds 1..K).
#   pass=K/K -> ceiling (useless for levers)
#   pass=0/K -> floor   (no lever can show lift either; the task is too hard)
#   else     -> HEADROOM (this is the measurable corpus)
#
# Run on the QUIESCED Pi (stop geist-home; thread count changes trajectories):
#   SPG_BIN=build/host-release/bin/geistshell MODEL=/path/model.gguf \
#     K=6 T=0.9 sh eval/bench_headroom.sh
set -eu

SPG=${SPG_BIN:-build/host-release/bin/geistshell}
MODEL=${MODEL:?set MODEL=/path/to/model.gguf}
K=${K:-6}
T=${T:-0.9}
T2=$(mktemp -d)
trap 'rm -rf "$T2"' EXIT

# Same competing-capability policy/scenario as the earlier goal benches.
printf '(scenario\n (host (id web) (criticality_bp 8000))\n (host (id db) (criticality_bp 10000))\n (service (id ssh_web) (host web) (name ssh) (port 22) (exposure_bp 5000))\n (account (id root_web) (host web) (username root) (enabled true))\n (credential (id key_root_web) (account root_web) (strength_bp 3000))\n (vulnerability (id cve_demo) (service ssh_web) (severity_bp 8000) (patched false))\n (network_edge (from web) (to db) (reachability_bp 5000)))\n' \
    > "$T2/scen.spg"

mkrun() { # $1 tag  $2 goal  $3 expect -> prints run.spg path
    f="$T2/run_$1.spg"
    printf '(run (model "%s") (policy "examples/policy.spg") (scenario "%s") (corpus "examples/corpus.spg") (journal "%s/j_%s.sgj") (seed 1)\n (budgets (inference_steps 6) (tokens 512) (shell_actions 2) (sim_actions 6) (wall_ms 240000))\n (goal "%s") (expect "%s"))\n' \
        "$MODEL" "$T2/scen.spg" "$T2" "$1" "$2" "$3" > "$f"
    printf '%s' "$f"
}

attempt() { # $1 run.spg  $2 temperature  $3 seed -> "pass" or "fail"
    OUT=$("$SPG" agent --config "$1" --constrained --allow-exec \
              --temperature "$2" --seed "$3" 2>/dev/null || true)
    v=$(printf '%s' "$OUT" | sed -n 's/^verdict=//p' | tail -1)
    [ "$v" = "pass" ] && printf 'pass' || printf 'fail'
}

# name|goal|expect  — expects deliberately NOT substrings of the goal wording,
# so echoing the goal back cannot satisfy the criterion.
TASKS='reverse|Run a local shell command that prints the word STONE reversed.|ENOTS
upper|Run a local shell command that prints the word cloud in capital letters.|CLOUD
count|Run a local shell command that prints the number of letters in the word LANTERN.|7
sum|Run a local shell command that prints the sum of 17 and 25.|42
seq|Run a local shell command that prints the numbers 1 to 5 on one line.|1 2 3 4 5
twice|Run a local shell command that prints the word MIRROR twice separated by a hyphen.|MIRROR-MIRROR'

printf '%-8s | %-6s | %-9s | %s\n' task greedy "sampled" verdict
printf -- '---------+--------+-----------+----------\n'
echo "$TASKS" | while IFS='|' read -r name goal expect; do
    [ -z "$name" ] && continue
    R=$(mkrun "g_$name" "$goal" "$expect")
    g=$(attempt "$R" 0 1)
    p=0; k=0
    while [ "$k" -lt "$K" ]; do
        k=$((k + 1))
        R=$(mkrun "s_${name}_$k" "$goal" "$expect")
        [ "$(attempt "$R" "$T" "$k")" = "pass" ] && p=$((p + 1))
    done
    if [ "$p" -eq "$K" ]; then v=ceiling
    elif [ "$p" -eq 0 ]; then v=floor
    else v=HEADROOM; fi
    printf '%-8s | %-6s | %4d/%-4d | %s\n' "$name" "$g" "$p" "$K" "$v"
done
printf -- '---------+--------+-----------+----------\n'
printf 'HEADROOM tasks (0 < pass < %d at T=%s) form the lever-measurable corpus.\n' "$K" "$T"
