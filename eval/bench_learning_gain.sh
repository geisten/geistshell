#!/bin/sh
# Benchmark (geistshell#25): learning gain per context-token, on a small model.
#
# bench_context.sh already measured the DENOMINATOR model-free: geistshell's
# per-tick learning cost is flat (one directive), the full-index RAG path grows.
# This script measures the NUMERATOR — does that flat-context learning actually
# lift task success? — and divides the two into the headline number.
#
# Three arms, same model, same seq-len budget, same corpus, as the lesson set
# accumulates across passes:
#
#   1. control     — no memory (the floor)
#   2. geistshell  — one budgeted directive per tick (P6): --directive-slug
#   3. full-index  — the whole mind-palace index every tick (the "context grows
#                    with learning" baseline): --memory-dir, no directive slug
#
# Per arm, per pass: task-success rate, and context tokens/tick (measured from
# the journaled MODEL_INPUT record, ~4 bytes/token — model-independent). The
# headline is success-lift-over-control divided by the tokens the arm added to
# the context.
#
# The PREREQUISITE the issue flags as the hard part: a corpus the small model
# can actually make progress on, each task verifiable via (expect ...), where a
# directive plausibly helps. The default here is the print-a-word corpus the
# goal-headroom work established (docs/LEARNING.md); override WORDS for your own.
#
# HONEST framing: this can REFUTE the SOTA claim. If learning does not lift
# success on a small model within a short window, the table says so, and the
# LEARNING.md SOTA paragraph should be retired rather than kept unproven.
#
# Model-gated, like every bench_*.sh: it needs a real GGUF and is NOT part of
# `make test`. Run on a quiesced host:
#   SPG_BIN=build/host-release/bin/geistshell MODEL=/path/model.gguf \
#     PASSES=3 sh eval/bench_learning_gain.sh
set -eu

SPG=${SPG_BIN:-build/host-release/bin/geistshell}
[ -x "$SPG" ] || SPG=build/host-debug/bin/geistshell
if [ -z "${MODEL:-}" ]; then
    echo "bench_learning_gain: skipped — set MODEL=/path/to/model.gguf"
    echo "  (the context-cost denominator is measured model-free by"
    echo "   eval/bench_context.sh; this arm needs real inference)"
    exit 0
fi

PASSES=${PASSES:-3}
T=${T:-0.8}
N=${N:-4}
WORDS=${WORDS:-"READY DONE OKAY FINE GOOD SET"}
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

# One shared scenario/policy: examples/policy.spg enables local_shell, so the
# task "print the word" is completable and a directive can nudge the finish.
SCEN="$W/scen.spg"
printf '(scenario (host (id h) (criticality_bp 5000)))\n' > "$SCEN"

# The directive under test: the goal-headroom lesson (print, then finish).
MEM="$W/mem"
printf 'After the goal word is printed, emit (kind finish) rather than repeating.\n' \
    | "$SPG" memory save lesson-goal \
        "After the goal word appears, emit (recommend (kind finish) ...)." \
        --dir "$MEM" >/dev/null

mkrun() { # $1 tag  $2 word -> run.spg path
    f="$W/run_$1.spg"
    printf '(run (model "%s") (policy "examples/policy.spg") (scenario "%s") (corpus "examples/corpus.spg") (journal "%s") (seed 1)\n (budgets (inference_steps 6) (tokens 512) (shell_actions 2) (sim_actions 0) (wall_ms 240000))\n (goal "Run a local shell command that prints the word %s.") (expect "%s"))\n' \
        "$MODEL" "$SCEN" "$W/j_$1.sgj" "$2" "$2" > "$f"
    printf '%s' "$f"
}

# Context tokens/tick from the journal: the first MODEL_INPUT payload size in
# bytes / 4. Model-independent and needs no new field.
ctx_tokens() { # $1 journal
    b=$(strings "$1" 2>/dev/null | awk '/\(contract/{print length($0); exit}')
    [ -z "$b" ] && b=0
    echo $(( (b + 3) / 4 ))
}

run_arm() { # $1 run.spg  $2 seed  $3... extra args -> "pass tokens"
    cfg=$1; seed=$2; shift 2
    OUT=$("$SPG" agent --config "$cfg" --constrained --allow-exec \
              --temperature "$T" --seed "$seed" --best-of "$N" "$@" \
              2>/dev/null || true)
    v=$(printf '%s' "$OUT" | sed -n 's/^verdict=//p' | tail -1)
    j=$(printf '%s' "$OUT" | sed -n 's/.*journal=\([^ ]*\).*/\1/p' | tail -1)
    p=0; [ "$v" = "pass" ] && p=1
    tk=0; [ -n "$j" ] && [ -f "$j" ] && tk=$(ctx_tokens "$j")
    printf '%d %d' "$p" "$tk"
}

printf '%-4s | %-18s | %-18s | %-18s\n' \
    pass "control pass/tok" "directive pass/tok" "full-index pass/tok"
printf -- '-----+--------------------+--------------------+-------------------\n'

pass=0
while [ "$pass" -lt "$PASSES" ]; do
    pass=$((pass + 1))
    cp=0; ct=0; dp=0; dt=0; fp=0; ft=0; n=0
    for word in $WORDS; do
        n=$((n + 1))
        R=$(mkrun "c_${pass}_${word}" "$word")
        set -- $(run_arm "$R" "$pass"); cp=$((cp + $1)); ct=$((ct + $2))
        R=$(mkrun "d_${pass}_${word}" "$word")
        set -- $(run_arm "$R" "$pass" --memory-dir "$MEM" --directive-slug lesson-goal)
        dp=$((dp + $1)); dt=$((dt + $2))
        R=$(mkrun "f_${pass}_${word}" "$word")
        set -- $(run_arm "$R" "$pass" --memory-dir "$MEM")
        fp=$((fp + $1)); ft=$((ft + $2))
    done
    # per-pass averages (integer tokens/tick)
    printf '%-4d | %6d/%-2d  %5d   | %6d/%-2d  %5d   | %6d/%-2d  %5d\n' \
        "$pass" "$cp" "$n" $((ct / n)) "$dp" "$n" $((dt / n)) \
        "$fp" "$n" $((ft / n))

    # Accumulate a fresh lesson each pass so the full-index arm's context grows
    # while the directive arm's stays flat — the whole point of the comparison.
    printf 'body %d\n' "$pass" | "$SPG" memory save "lesson-extra-$pass" \
        "Pass $pass: keep actions minimal and finish as soon as the goal shows." \
        --dir "$MEM" >/dev/null
done
printf -- '-----+--------------------+--------------------+-------------------\n'
cat <<'NOTE'
Reading:
  * directive tokens/tick stays FLAT across passes; full-index grows.
  * gain-per-token = (arm pass-rate - control pass-rate) / (arm tokens - control tokens).
    A positive directive gain at flat cost is the SOTA-for-small-seq-len claim;
    a null result RETIRES that paragraph in docs/LEARNING.md rather than keeping
    it unproven. Report whatever the numbers say.
NOTE
