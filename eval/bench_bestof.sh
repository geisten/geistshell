#!/bin/sh
# Verifier-guided best-of-N vs greedy (geistshell, constrained decode).
#
# geistshell already has a verifier (the policy gate + expect + the world). This
# harness runs each task two ways and reports whether the verifier-picked
# best-of-N beats a single greedy trajectory:
#
#   greedy:    --temperature 0            (one deterministic run)
#   best-of-N: --temperature T, seeds 1..N, STOP at the first verdict=pass
#
# Under constrained decode the masked choice slots (kind, capability) sample at
# T>0 (see model_adapter.c), so different seeds explore different valid
# DECISIONS; the free slots vary too. The verifier keeps the run that passes.
#
# Two corpora, because best-of-N only helps where greedy is "sometimes right":
#   A) simulator scenarios — greedy already lands the right kind; expect ~no lift.
#   B) a shell goal under a policy that ALSO enables the simulator — greedy may
#      pick the wrong kind; best-of-N can explore to the working one.
#
# Run on the Pi:
#   SPG_BIN=build/host-release/bin/geistshell MODEL=/path/model.gguf N=5 T=0.8 \
#     sh eval/bench_bestof.sh
set -eu

SPG=${SPG_BIN:-build/host-release/bin/geistshell}
MODEL=${MODEL:?set MODEL=/path/to/model.gguf}
N=${N:-5}
T=${T:-0.8}
T2=$(mktemp -d)
trap 'rm -rf "$T2"' EXIT

# One attempt: prints "pass" or "fail" from the verdict line.
attempt() { # $1 run.spg  $2 temperature  $3 seed
    OUT=$("$SPG" agent --config "$1" --constrained --allow-exec \
              --temperature "$2" --seed "$3" 2>/dev/null || true)
    printf '%s' "$OUT" | sed -n 's/.*verdict=\([a-z_]*\).*/\1/p' | tail -1
}

# greedy = one run at T=0; best-of-N = up to N seeds at T, first pass wins.
run_task() { # $1 run.spg -> prints "greedy_pass bestof_pass attempts_used"
    g=$(attempt "$1" 0 1); gp=0; [ "$g" = "pass" ] && gp=1
    bp=0; used=0; k=0
    while [ "$k" -lt "$N" ]; do
        k=$((k + 1)); used=$k
        v=$(attempt "$1" "$T" "$k")
        if [ "$v" = "pass" ]; then bp=1; break; fi
    done
    printf '%d %d %d' "$gp" "$bp" "$used"
}

mkrun() { # $1 journal $2 scenario $3 policy $4 goal-line -> writes run.spg, prints path
    f="$T2/$(basename "$1").spg"
    printf '(run (model "%s") (policy "%s") (scenario "%s") (corpus "examples/corpus.spg") (journal "%s") (seed 1)\n (budgets (inference_steps 6) (tokens 512) (shell_actions 2) (sim_actions 6) (wall_ms 240000))\n %s (expect "%s"))\n' \
        "$MODEL" "$3" "$2" "$1" "$4" "$5" > "$f"
    printf '%s' "$f"
}

# --- corpus A: simulator scenarios (rising severity) ---
mk_scen() { printf '(scenario\n (host (id web) (criticality_bp 8000))\n (host (id db) (criticality_bp 10000))\n (service (id ssh_web) (host web) (name ssh) (port 22) (exposure_bp 5000))\n (account (id root_web) (host web) (username root) (enabled true))\n (credential (id key_root_web) (account root_web) (strength_bp 3000))\n (vulnerability (id cve_demo) (service ssh_web) (severity_bp %s) (patched false))\n (network_edge (from web) (to db) (reachability_bp 5000)))\n' "$1" > "$2"; }

# corpus B: examples/policy.spg already enables BOTH simulator and local_shell,
# so it is the competing-capability policy (an earlier custom one had a schema
# error and instant-failed every run -> a false 0/N).
POLB=examples/policy.spg

printf '%-22s | %-6s | %-9s | %-8s\n' task greedy best-of-N attempts
printf -- '-----------------------+--------+-----------+---------\n'
ga=0; ba=0; n=0
row() { printf '%-22s | %-6s | %-9s | %-8s\n' "$1" "$2" "$3" "$4"; }

for sev in 4000 8000; do
    mk_scen "$sev" "$T2/scA_$sev.spg"
    R=$(mkrun "$T2/jA_$sev" "$T2/scA_$sev.spg" "examples/policy.spg" "" "sim_result")
    set -- $(run_task "$R"); n=$((n+1)); ga=$((ga+$1)); ba=$((ba+$2))
    row "simA sev=$sev" "$([ "$1" = 1 ] && echo pass || echo fail)" "$([ "$2" = 1 ] && echo pass || echo fail)" "$3"
done

# corpus B: shell goal, both kinds enabled -> greedy may mis-pick the kind.
mk_scen 8000 "$T2/scB.spg"
for word in READY DONE OKAY; do
    R=$(mkrun "$T2/jB_$word" "$T2/scB.spg" "$POLB" "(goal \"Run a local shell command that prints the word $word.\")" "$word")
    set -- $(run_task "$R"); n=$((n+1)); ga=$((ga+$1)); ba=$((ba+$2))
    row "shellB goal=$word" "$([ "$1" = 1 ] && echo pass || echo fail)" "$([ "$2" = 1 ] && echo pass || echo fail)" "$3"
done

printf -- '-----------------------+--------+-----------+---------\n'
printf 'TOTAL greedy=%d/%d  best-of-%d=%d/%d\n' "$ga" "$n" "$N" "$ba" "$n"
