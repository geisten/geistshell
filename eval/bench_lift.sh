#!/bin/sh
# LIFT harness for constrained decoding (geistshell#34 stage 3; #25 numerator).
#
# geistshell has no free-text goal — the task IS the security scenario, and the
# model recommends risk-reducing actions. So the corpus is four scenario
# variants (rising vulnerability severity = four risk postures). Each is run
# twice against the REAL model:
#
#   FREE:        agent --config <run>            (free decode)
#   CONSTRAINED: agent --config <run> --constrained
#
# Three measured columns per arm:
#   valid  — the model emitted a valid, policy-allowed recommendation
#            (the journal carries a (policy_decision (decision allow ...)).
#   acted  — an action actually executed and changed state (a (sim_result ...)
#            in the journal). This is task success on the world: risk moved.
#   done   — verdict=pass. NOTE the CLI's expect judge additionally requires
#            termination=FINISHED (main.c), i.e. the model must emit
#            (kind finish). Greedy Gemma keeps choosing simulator until the step
#            cap, so `done` stays 0 even when `acted` is 1 — emitting a valid
#            action and choosing to STOP are separate behaviours. `acted` is the
#            honest task-success signal; `done` is reported for completeness.
#
# Headline: free scores ~0 (cannot emit the DSL); constrained lifts valid+acted.
# This is the #25 numerator — the success lift the flat-context claim needs — on
# the model that could not act before. Run on the Pi where the GGUF lives:
#   SPG_BIN=build/host-release/bin/geistshell MODEL=/path/model.gguf sh eval/bench_lift.sh
set -eu

SPG=${SPG_BIN:-build/host-release/bin/geistshell}
MODEL=${MODEL:?set MODEL=/path/to/model.gguf}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

# Four scenarios differing only in the vulnerability severity -> four tasks.
SEVERITIES="4000 6000 8000 9500"

scenario_file() { # $1 = severity -> writes a scenario, prints its path
    f="$T/scen_$1.spg"
    printf '(scenario\n (host (id web) (criticality_bp 8000))\n (host (id db) (criticality_bp 10000))\n (service (id ssh_web) (host web) (name ssh) (port 22) (exposure_bp 5000))\n (account (id root_web) (host web) (username root) (enabled true))\n (credential (id key_root_web) (account root_web) (strength_bp 3000))\n (vulnerability (id cve_demo) (service ssh_web) (severity_bp %s) (patched false))\n (network_edge (from web) (to db) (reachability_bp 5000)))\n' "$1" > "$f"
    printf '%s' "$f"
}

run_arm() { # $1 = scenario path, $2 = journal, $3 = extra flag ("" or --constrained)
    SCEN=$1; J=$2; FLAG=$3
    printf '(run\n (model "%s")\n (policy "examples/policy.spg")\n (scenario "%s")\n (corpus "examples/corpus.spg")\n (journal "%s")\n (seed 42)\n (budgets (inference_steps 3) (tokens 512) (shell_actions 1) (sim_actions 4) (wall_ms 120000) (journal_bytes 1048576) (risk_bp 10000))\n (expect "sim_result"))\n' "$MODEL" "$SCEN" "$J" > "$T/run.spg"
    # shellcheck disable=SC2086
    OUT=$("$SPG" agent --config "$T/run.spg" --allow-exec $FLAG 2>/dev/null || true)
    VERDICT=$(printf '%s' "$OUT" | sed -n 's/.*verdict=\([a-z]*\).*/\1/p' | tail -1)
    # a valid, policy-allowed form is one that reached the gate with an allow.
    strings "$J" 2>/dev/null | grep -q '(policy_decision (decision allow' && VALID=1 || VALID=0
    # an executed action left a sim_result in the journal.
    strings "$J" 2>/dev/null | grep -q '(sim_result ' && ACTED=1 || ACTED=0
    [ "$VERDICT" = "pass" ] && DONE=1 || DONE=0
    printf '%d %d %d' "$VALID" "$ACTED" "$DONE"
}

row() { printf '%-9s | %-4s %-6s | %-4s %-6s | %-4s %-5s\n' "$1" "$2" "$3" "$4" "$5" "$6" "$7"; }
row task "free" "constr" "free" "constr" "free" "constr"
row ""   "valid" "valid"  "acted" "acted" "done" "done"
printf -- '----------+-------------+-------------+------------\n'
fv=0; cv=0; fa=0; ca=0; fd=0; cd=0; n=0
for sev in $SEVERITIES; do
    n=$((n + 1))
    SCEN=$(scenario_file "$sev")
    set -- $(run_arm "$SCEN" "$T/free_$sev.sgj" "");            FV=$1; FA=$2; FD=$3
    set -- $(run_arm "$SCEN" "$T/constr_$sev.sgj" "--constrained"); CV=$1; CA=$2; CD=$3
    fv=$((fv+FV)); cv=$((cv+CV)); fa=$((fa+FA)); ca=$((ca+CA)); fd=$((fd+FD)); cd=$((cd+CD))
    row "sev=$sev" "$FV" "$CV" "$FA" "$CA" "$FD" "$CD"
done
printf -- '----------+-------------+-------------+------------\n'
row HEADLINE "$fv/$n" "$cv/$n" "$fa/$n" "$ca/$n" "$fd/$n" "$cd/$n"
