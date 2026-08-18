#!/bin/sh
# Directive x best-of-N (Hebel 1): does a stored lesson's directive shift
# sampling probability mass, even if it cannot flip the greedy argmax?
#
# Every prior directive probe ran greedy (LEARNING.md: Weg 1 "changes nothing"),
# where the directive had to FLIP the argmax to show at all. Under temperature
# sampling it only has to SHIFT mass; the verifier (best-of-N, first pass wins)
# amplifies any shift into a measurable difference in attempts-to-pass.
#
#   control: agent --best-of N --temperature T                  (no lesson)
#   learned: same + --memory-dir M --directive-slug lesson-goal (directive
#            rendered every step, the Weg-1 strong channel)
#
# Metric per task: pass + attempts_used (fails are censored at N).
#   learned < control attempts  -> the directive shifts mass; lessons act as a
#                                  sampling prior, not a command.
#   no difference               -> the prompt channel is dead on this model.
#
# Run on the QUIESCED Pi (stop geist --serve; thread count changes trajectories):
#   SPG_BIN=build/host-release/bin/geistshell MODEL=/path/model.gguf \
#     N=6 T=0.9 REPS=3 sh eval/bench_directive.sh
set -eu

SPG=${SPG_BIN:-build/host-release/bin/geistshell}
MODEL=${MODEL:?set MODEL=/path/to/model.gguf}
N=${N:-6}
T=${T:-0.9}
REPS=${REPS:-3}
WORDS=${WORDS:-"READY DONE OKAY FINE GOOD SET"}
T2=$(mktemp -d)
trap 'rm -rf "$T2"' EXIT

# The lesson under test: the exact decision headroom the goal task measured
# (model prints the word but never emits finish). Directive = description.
MEM="$T2/mem"
printf 'After the goal word has been printed, emit (kind finish) instead of repeating the command.\n' \
    | "$SPG" memory save lesson-goal \
        "After the goal word appears in the observation, emit (kind finish)." \
        --dir "$MEM" >/dev/null

# Same scenario/policy as bench_bestof corpus B: examples/policy.spg enables
# BOTH simulator and local_shell, so the model's own kind-choice decides.
printf '(scenario\n (host (id web) (criticality_bp 8000))\n (host (id db) (criticality_bp 10000))\n (service (id ssh_web) (host web) (name ssh) (port 22) (exposure_bp 5000))\n (account (id root_web) (host web) (username root) (enabled true))\n (credential (id key_root_web) (account root_web) (strength_bp 3000))\n (vulnerability (id cve_demo) (service ssh_web) (severity_bp 8000) (patched false))\n (network_edge (from web) (to db) (reachability_bp 5000)))\n' \
    > "$T2/scen.spg"

mkrun() { # $1 tag  $2 word -> prints run.spg path
    f="$T2/run_$1.spg"
    printf '(run (model "%s") (policy "examples/policy.spg") (scenario "%s") (corpus "examples/corpus.spg") (journal "%s") (seed 1)\n (budgets (inference_steps 6) (tokens 512) (shell_actions 2) (sim_actions 6) (wall_ms 240000))\n (goal "Run a local shell command that prints the word %s.") (expect "%s"))\n' \
        "$MODEL" "$T2/scen.spg" "$T2/j_$1.sgj" "$2" "$2" > "$f"
    printf '%s' "$f"
}

run_arm() { # $1 run.spg  $2 seed  $3... extra agent args -> "pass attempts"
    cfg=$1; seed=$2; shift 2
    OUT=$("$SPG" agent --config "$cfg" --constrained --allow-exec \
              --temperature "$T" --seed "$seed" --best-of "$N" "$@" 2>/dev/null || true)
    v=$(printf '%s' "$OUT" | sed -n 's/^verdict=//p' | tail -1)
    a=$(printf '%s' "$OUT" | sed -n 's/.*attempts_used=\([0-9]*\).*/\1/p' | tail -1)
    [ -z "$a" ] && a=$N
    p=0; [ "$v" = "pass" ] && p=1
    printf '%d %d' "$p" "$a"
}

printf '%-6s %-4s | %-14s | %-14s\n' word rep "ctrl pass/att" "lesson pass/att"
printf -- '------------+----------------+---------------\n'
cp=0; ca=0; lp=0; la=0; n=0
for word in $WORDS; do
    r=0
    while [ "$r" -lt "$REPS" ]; do
        r=$((r + 1))
        R=$(mkrun "c_${word}_$r" "$word")
        set -- $(run_arm "$R" "$r"); cpi=$1; cai=$2
        R=$(mkrun "l_${word}_$r" "$word")
        set -- $(run_arm "$R" "$r" --memory-dir "$MEM" --directive-slug lesson-goal)
        lpi=$1; lai=$2
        n=$((n+1)); cp=$((cp+cpi)); ca=$((ca+cai)); lp=$((lp+lpi)); la=$((la+lai))
        printf '%-6s %-4s | %6d / %-5d | %6d / %-5d\n' "$word" "$r" "$cpi" "$cai" "$lpi" "$lai"
    done
done
printf -- '------------+----------------+---------------\n'
printf 'TOTAL over %d runs (N=%d T=%s):\n' "$n" "$N" "$T"
printf '  control: pass=%d/%d  attempts_sum=%d\n' "$cp" "$n" "$ca"
printf '  lesson:  pass=%d/%d  attempts_sum=%d\n' "$lp" "$n" "$la"
printf 'Fewer lesson attempts at equal pass = the directive shifts sampling mass.\n'
