#!/bin/sh
# Does the endpoint-sensitive verifier cause the shell corpus 0/N, or the model?
#
# bench_bestof.sh judged pass = the FINAL observation contains the expect word.
# The shell-goal corpus stayed 0/N even at N=8. This harness re-judges the same
# runs with a STICKY criterion: pass = the word appears in ANY observation in the
# journal (the model produced it at some step), not only the last. It prints both
# so the gap is visible:
#
#   endpoint: verdict=pass from the CLI (last observation contains the word)
#   sticky:   the word appears anywhere in the run's journal
#
# If sticky passes where endpoint fails -> the criterion was the culprit, and a
# less endpoint-sensitive verifier is the fix (not more samples). If sticky also
# fails -> the model never does the shell action; the competing capability is.
#
#   SPG_BIN=... MODEL=... N=8 T=0.9 sh eval/bench_sticky.sh
set -eu

SPG=${SPG_BIN:-build/host-release/bin/geistshell}
MODEL=${MODEL:?set MODEL=/path/to/model.gguf}
N=${N:-8}
T=${T:-0.9}
D=$(mktemp -d); trap 'rm -rf "$D"' EXIT

mk_scen() { printf '(scenario\n (host (id web) (criticality_bp 8000))\n (host (id db) (criticality_bp 10000))\n (service (id ssh_web) (host web) (name ssh) (port 22) (exposure_bp 5000))\n (account (id root_web) (host web) (username root) (enabled true))\n (credential (id key_root_web) (account root_web) (strength_bp 3000))\n (vulnerability (id cve_demo) (service ssh_web) (severity_bp 8000) (patched false))\n (network_edge (from web) (to db) (reachability_bp 5000)))\n' > "$1"; }
mk_scen "$D/sc.spg"
# examples/policy.spg already enables BOTH simulator (sim.act) and local_shell
# (build.run) -> the kind is a real choice, no custom (broken) policy needed.
POL=examples/policy.spg

# one attempt: prints "<endpoint> <sticky>" as 0/1, where sticky = word in journal
attempt() { # $1 goalword $2 temp $3 seed
    J="$D/j.sgj"
    printf '(run (model "%s") (policy "%s") (scenario "%s") (corpus "examples/corpus.spg") (journal "%s") (seed 1)\n (budgets (inference_steps 6) (tokens 512) (shell_actions 2) (sim_actions 6) (wall_ms 240000))\n (goal "Run a local shell command that prints the word %s.") (expect "%s"))\n' \
        "$MODEL" "$POL" "$D/sc.spg" "$J" "$1" "$1" > "$D/run.spg"
    V=$("$SPG" agent --config "$D/run.spg" --constrained --allow-exec \
            --temperature "$2" --seed "$3" 2>/dev/null | sed -n 's/.*verdict=\([a-z_]*\).*/\1/p' | tail -1 || true)
    ep=0; [ "$V" = "pass" ] && ep=1
    st=0; strings "$J" 2>/dev/null | grep -q "$1" && st=1
    printf '%d %d' "$ep" "$st"
}

printf '%-14s | %-16s | %-18s | %-8s\n' goal "greedy ep/sticky" "best-of-N ep/sticky" attempts
printf -- '---------------+------------------+--------------------+---------\n'
gep=0; gst=0; bep=0; bst=0; n=0
for w in READY DONE OKAY; do
    n=$((n+1))
    set -- $(attempt "$w" 0 1); GEP=$1; GST=$2
    # best-of-N: stop at the first STICKY pass (word produced anywhere)
    BEP=0; BST=0; used=0; k=0
    while [ "$k" -lt "$N" ]; do
        k=$((k+1)); used=$k
        set -- $(attempt "$w" "$T" "$k")
        [ "$1" = 1 ] && BEP=1
        if [ "$2" = 1 ]; then BST=1; break; fi
    done
    gep=$((gep+GEP)); gst=$((gst+GST)); bep=$((bep+BEP)); bst=$((bst+BST))
    printf '%-14s | %-16s | %-18s | %-8d\n' "$w" "$GEP/$GST" "$BEP/$BST" "$used"
done
printf -- '---------------+------------------+--------------------+---------\n'
printf 'TOTAL  greedy ep=%d/%d sticky=%d/%d   best-of-%d ep=%d/%d sticky=%d/%d\n' \
    "$gep" "$n" "$gst" "$n" "$N" "$bep" "$n" "$bst" "$n"
