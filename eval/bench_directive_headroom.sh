#!/bin/sh
# Hebel 1 on the measured-headroom corpus: does a recipe-carrying directive
# shift sampling mass where control demonstrably fails?
#
# The headroom census (LEARNING.md 2026-08-16) found exactly two tasks in the
# band 0 < pass < K: literal composition (seq "1 2 3 4 5", twice
# "MIRROR-MIRROR"), control ~1/6 per attempt. Here each task gets a lesson
# whose description IS the exact recipe, rendered as the Weg-1 strong channel
# `(directive "...")` every step. If the directive shifts mass, the lesson arm
# passes earlier (fewer attempts) or more often; if the arms tie, the prompt
# channel is dead even with the answer spelled out.
#
#   control: agent --best-of N --temperature T
#   learned: same + --memory-dir M --directive-slug lesson-<task>
#
# Run on the QUIESCED Pi (stop geist-home):
#   SPG_BIN=build/host-release/bin/geistshell MODEL=/path/model.gguf \
#     N=6 T=0.9 REPS=3 sh eval/bench_directive_headroom.sh
set -eu

SPG=${SPG_BIN:-build/host-release/bin/geistshell}
MODEL=${MODEL:?set MODEL=/path/to/model.gguf}
N=${N:-6}
T=${T:-0.9}
REPS=${REPS:-3}
T2=$(mktemp -d)
trap 'rm -rf "$T2"' EXIT

MEM="$T2/mem"
printf 'The goal is satisfied by exactly this command.\n' | "$SPG" memory save lesson-seq \
    "Emit the shell command: echo 1 2 3 4 5" --dir "$MEM" >/dev/null
printf 'The goal is satisfied by exactly this command.\n' | "$SPG" memory save lesson-twice \
    "Emit the shell command: echo MIRROR-MIRROR" --dir "$MEM" >/dev/null

printf '(scenario\n (host (id web) (criticality_bp 8000))\n (host (id db) (criticality_bp 10000))\n (service (id ssh_web) (host web) (name ssh) (port 22) (exposure_bp 5000))\n (account (id root_web) (host web) (username root) (enabled true))\n (credential (id key_root_web) (account root_web) (strength_bp 3000))\n (vulnerability (id cve_demo) (service ssh_web) (severity_bp 8000) (patched false))\n (network_edge (from web) (to db) (reachability_bp 5000)))\n' \
    > "$T2/scen.spg"

mkrun() { # $1 tag  $2 goal  $3 expect -> prints run.spg path
    f="$T2/run_$1.spg"
    printf '(run (model "%s") (policy "examples/policy.spg") (scenario "%s") (corpus "examples/corpus.spg") (journal "%s/j_%s.sgj") (seed 1)\n (budgets (inference_steps 6) (tokens 512) (shell_actions 2) (sim_actions 6) (wall_ms 240000))\n (goal "%s") (expect "%s"))\n' \
        "$MODEL" "$T2/scen.spg" "$T2" "$1" "$2" "$3" > "$f"
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

TASKS='seq|Run a local shell command that prints the numbers 1 to 5 on one line.|1 2 3 4 5
twice|Run a local shell command that prints the word MIRROR twice separated by a hyphen.|MIRROR-MIRROR'

printf '%-6s %-4s | %-14s | %-14s\n' task rep "ctrl pass/att" "lesson pass/att"
printf -- '------------+----------------+---------------\n'
cp=0; ca=0; lp=0; la=0; n=0
echo "$TASKS" | while IFS='|' read -r name goal expect; do
    [ -z "$name" ] && continue
    r=0
    while [ "$r" -lt "$REPS" ]; do
        r=$((r + 1))
        R=$(mkrun "c_${name}_$r" "$goal" "$expect")
        set -- $(run_arm "$R" "$r"); cpi=$1; cai=$2
        R=$(mkrun "l_${name}_$r" "$goal" "$expect")
        set -- $(run_arm "$R" "$r" --memory-dir "$MEM" --directive-slug "lesson-$name")
        lpi=$1; lai=$2
        printf '%-6s %-4s | %6d / %-5d | %6d / %-5d\n' "$name" "$r" "$cpi" "$cai" "$lpi" "$lai"
        # totals accumulate inside the subshell; print a running summary line
        # after the last row so the caller can grep it.
        cp=$((cp+cpi)); ca=$((ca+cai)); lp=$((lp+lpi)); la=$((la+lai)); n=$((n+1))
        printf 'TOTALS n=%d control pass=%d att=%d | lesson pass=%d att=%d\n' \
            "$n" "$cp" "$ca" "$lp" "$la"
    done
done
printf 'Fewer lesson attempts (or more passes) at N=%d T=%s = the directive shifts mass.\n' "$N" "$T"
