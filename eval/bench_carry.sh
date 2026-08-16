#!/bin/sh
# In-conversation learning (Hebel 1): a turn that the verifier passes contributes
# its emitted (recommend ...) form as a few-shot exemplar to the LATER turns of
# the same session. The bet is the one this project's evidence supports: a small
# model imitates concrete, recent, VERIFIED examples far better than it follows
# an abstract directive (which did not lift it at all).
#
# No engine change — it reuses the existing (examples ...) context slot
# (--exemplars). Constrained decode is on, so early turns pass and produce the
# verified exemplars that later turns carry.
#
#   no-carry: each goal runs with an empty exemplar set.
#   carry:    each passing goal's emitted form is appended to the exemplar file
#             that subsequent goals see.
#
# Reports the pass rate over the sequence for both arms. Run on a QUIESCED box
# (the geist --serve daemon disabled) — greedy decode is thread-count sensitive.
#   SPG_BIN=... MODEL=... sh eval/bench_carry.sh
set -eu

SPG=${SPG_BIN:-build/host-release/bin/geistshell}
MODEL=${MODEL:?set MODEL=/path/to/model.gguf}
D=$(mktemp -d); trap 'rm -rf "$D"' EXIT

printf '(scenario (host (id web) (criticality_bp 8000)) (service (id ssh_web) (host web) (name ssh) (port 22) (exposure_bp 5000)) (vulnerability (id cve_demo) (service ssh_web) (severity_bp 8000) (patched false)))\n' > "$D/sc.spg"

# a sequence of same-shape goals (print a word via local_shell)
GOALS="READY DONE OKAY FINE GOOD SET"

run_one() { # $1 goalword $2 exemplar-file-or-empty $3 journal -> prints verdict
    if [ -n "$2" ] && [ -s "$2" ]; then EX="--exemplars $2"; else EX=""; fi
    printf '(run (model "%s") (policy "examples/policy.spg") (scenario "%s") (corpus "examples/corpus.spg") (journal "%s") (seed 1)\n (budgets (inference_steps 6) (tokens 512) (shell_actions 2) (sim_actions 6) (wall_ms 240000))\n (goal "Run a local shell command that prints the word %s.") (expect "%s"))\n' \
        "$MODEL" "$D/sc.spg" "$3" "$1" "$1" > "$D/run.spg"
    # shellcheck disable=SC2086
    "$SPG" agent --config "$D/run.spg" --constrained --allow-exec $EX 2>/dev/null \
        | sed -n 's/.*verdict=\([a-z_]*\).*/\1/p' | tail -1
}

# extract the model's emitted (recommend ...) form — one with a CONCRETE kind,
# not the (kind <a|b|c>) schema template that also appears in the journal.
extract_form() { # $1 journal
    strings "$1" 2>/dev/null | grep -m1 -oE \
        '\(recommend \(kind[[:space:]]*(local_shell|simulator|ssh_auth_probe|memory_save|memory_delete|memory_read|finish)\)[^~]{0,240}' \
        | sed -E 's/(.*\)\)).*/\1/'  # trim to the last )) -> a clean closed form
}

run_seq() { # $1 = carry(1/0) -> prints "pass_count detail"
    EX="$D/ex_$1.spg"; : > "$EX"
    pass=0; detail=""
    for w in $GOALS; do
        J="$D/j_$1_$w.sgj"
        [ "$1" = 1 ] && v=$(run_one "$w" "$EX" "$J") || v=$(run_one "$w" "" "$J")
        if [ "$v" = "pass" ]; then
            pass=$((pass+1)); detail="$detail $w:P"
            if [ "$1" = 1 ]; then
                f=$(extract_form "$J")
                [ -n "$f" ] && printf '  %s\n' "$f" >> "$EX"
            fi
        else
            detail="$detail $w:."
        fi
    done
    printf '%d |%s' "$pass" "$detail"
}

n=$(printf '%s\n' $GOALS | wc -l | tr -d ' ')
echo "sequence: $GOALS"
printf 'no-carry: %s\n' "$(run_seq 0)"
printf 'carry   : %s\n' "$(run_seq 1)"
echo "(P = verdict pass, . = fail; carry appends each passing form to later turns' exemplars)"
echo CARRY_DONE
