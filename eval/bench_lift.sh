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
#   acted  — a (sim_result ...) appears in the journal. NOTE this measures
#            SIMULATOR success, not task success: a run that acts through
#            local_shell scores 0 here despite having acted.
#   done   — verdict=pass: the run finished AND (expect "sim_result") was met.
#            Until 2026-08-09 the marker was matched against the FINAL
#            observation only, so a run that simulated and then took one more
#            shell step ended on "exit 0" and scored fail_observation with
#            termination=finished and acted=1 — `done` really measured "the last
#            observation happened to be a sim_result" and read 0/4. The loop now
#            latches the marker on every step (src/run/agent_loop.c,
#            observation_marker); the identical runs (Gemma 4 E2B Q4_K_M, M1 Max,
#            seed 42) score 4/4.
#   term   — why the run stopped, straight from the CLI. THIS is how to answer
#            "was the budget the cap?" (budget/max_steps = yes, finished = no).
#            Do not sweep the budget to find out: the budget block is rendered
#            into the context (src/context/context.c render_budgets), so raising
#            it changes the prompt. Changing only wall_ms — at the time a budget
#            that was not even enforced — moved acted from 4/4 to 0/4.
#
# Headline: free scores ~0 (cannot emit the DSL); constrained lifts valid+acted.
# This is the #25 numerator — the success lift the flat-context claim needs — on
# the model that could not act before. Run on the Pi where the GGUF lives:
#   SPG_BIN=build/host-release/bin/geistshell MODEL=/path/model.gguf sh eval/bench_lift.sh
set -eu

SPG=${SPG_BIN:-build/host-release/bin/geistshell}
MODEL=${MODEL:?set MODEL=/path/to/model.gguf}
# The step budget is the one knob that plausibly caps `done`: a model that keeps
# choosing simulator runs out of steps before it ever emits (kind finish). Left
# overridable so "is `done` model behaviour or budget?" is a measurement rather
# than an argument. Tokens/sim_actions/wall scale with it or they become the cap.
STEPS=${STEPS:-6}
TOKENS=${TOKENS:-512}
SIM_ACTIONS=${SIM_ACTIONS:-6}
WALL_MS=${WALL_MS:-240000}
# ARMS=both|constrained. The free arm is 0 in every condition measured so far, so
# a budget sweep can skip it and halve the wall clock. Default stays both.
ARMS=${ARMS:-both}
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
    printf '(run\n (model "%s")\n (policy "examples/policy.spg")\n (scenario "%s")\n (corpus "examples/corpus.spg")\n (journal "%s")\n (seed 42)\n (budgets (inference_steps %s) (tokens %s) (shell_actions 1) (sim_actions %s) (wall_ms %s))\n (expect "sim_result"))\n' "$MODEL" "$SCEN" "$J" "$STEPS" "$TOKENS" "$SIM_ACTIONS" "$WALL_MS" > "$T/run.spg"
    # shellcheck disable=SC2086
    OUT=$("$SPG" agent --config "$T/run.spg" --allow-exec $FLAG 2>/dev/null || true)
    VERDICT=$(printf '%s' "$OUT" | sed -n 's/.*verdict=\([a-z]*\).*/\1/p' | tail -1)
    # a valid, policy-allowed form is one that reached the gate with an allow.
    strings "$J" 2>/dev/null | grep -q '(policy_decision (decision allow' && VALID=1 || VALID=0
    # an executed action left a sim_result in the journal.
    strings "$J" 2>/dev/null | grep -q '(sim_result ' && ACTED=1 || ACTED=0
    [ "$VERDICT" = "pass" ] && DONE=1 || DONE=0
    # Why the run stopped. This is the direct answer to "was the budget the cap?"
    # — `budget`/`max_steps` mean yes, `finished` means the model chose to stop
    # with budget to spare. Reading it off a run that already happened beats
    # sweeping the budget, because the budget block is rendered into the context
    # (src/context/context.c render_budgets), so raising it changes the prompt
    # and the sweep measures a different task.
    TERM=$(printf '%s' "$OUT" | sed -n 's/.*termination=\([a-z_]*\).*/\1/p' | tail -1)
    printf '%d %d %d %s' "$VALID" "$ACTED" "$DONE" "${TERM:-none}"
}

row() { printf '%-9s | %-4s %-6s | %-4s %-6s | %-4s %-5s\n' "$1" "$2" "$3" "$4" "$5" "$6" "$7"; }
row task "free" "constr" "free" "constr" "free" "constr"
row ""   "valid" "valid"  "acted" "acted" "done" "done"
printf -- '----------+-------------+-------------+------------\n'
fv=0; cv=0; fa=0; ca=0; fd=0; cd=0; n=0; cterm=""; ccapped=0
for sev in $SEVERITIES; do
    n=$((n + 1))
    SCEN=$(scenario_file "$sev")
    if [ "$ARMS" = both ]; then
        set -- $(run_arm "$SCEN" "$T/free_$sev.sgj" ""); FV=$1; FA=$2; FD=$3
    else
        FV=-; FA=-; FD=-
    fi
    set -- $(run_arm "$SCEN" "$T/constr_$sev.sgj" "--constrained")
    CV=$1; CA=$2; CD=$3; CT=$4
    cterm="$cterm $CT"
    case "$CT" in budget|max_steps) ccapped=$((ccapped + 1));; esac
    if [ "$ARMS" = both ]; then
        fv=$((fv+FV)); fa=$((fa+FA)); fd=$((fd+FD))
    fi
    cv=$((cv+CV)); ca=$((ca+CA)); cd=$((cd+CD))
    row "sev=$sev" "$FV" "$CV" "$FA" "$CA" "$FD" "$CD"
done
printf -- '----------+-------------+-------------+------------\n'
# An arm that did not run reports "-", never "0/n": a zero that was never
# measured is indistinguishable from a measured failure in the output.
if [ "$ARMS" = both ]; then
    row HEADLINE "$fv/$n" "$cv/$n" "$fa/$n" "$ca/$n" "$fd/$n" "$cd/$n"
else
    row HEADLINE "-" "$cv/$n" "-" "$ca/$n" "-" "$cd/$n"
fi
printf '\ntermination (constrained arm): %s\n' "$cterm"
printf 'budget-capped runs: %s/%s   (finished = the budget was not the cap)\n' \
    "$ccapped" "$n"
