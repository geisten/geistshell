#!/bin/sh
# Real-model benchmark (geistshell#52). Run via `make bench`.
#
# Unlike improve_benchmark.sh, which drives deterministic scripted fakes and is
# part of `make test`, this one needs a GGUF and takes minutes. It is therefore
# NOT in `make test`: a fresh checkout would be red, and a multi-GB download per
# commit is not a build dependency.
#
# The other half of that decision matters just as much: a missing model is
# reported as **skipped**, exit 0. A benchmark that breaks the build when an
# optional artefact is absent gets commented out within a week, and then it is
# not a benchmark, it is a dead file.
#
# Usage:
#   make bench
#   make bench BENCH_MODEL=path/to/model.gguf BENCH_SAMPLES=5 BENCH_TEMP=0.8
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}
SUITE=${BENCH_SUITE:-examples/eval/bench/model_suite.spg}
RUN_CFG=${BENCH_RUN:-examples/eval/bench/model_run.spg}
SAMPLES=${BENCH_SAMPLES:-3}
TEMP=${BENCH_TEMP:-0.8}

skip() {
    echo "bench: skipped — $1"
    exit 0
}

[ -x "$SPG_BIN" ] || skip "$SPG_BIN not built (run make first)"
[ -r "$SUITE" ] || skip "no suite at $SUITE"
[ -r "$RUN_CFG" ] || skip "no run config at $RUN_CFG"

# The model path lives in the run config's (model "...") — eval reads it from
# there, not from the environment. An override therefore has to rewrite the
# config, not export a variable.
CFG_MODEL=$(sed -n 's/^[[:space:]]*(model "\(.*\)").*/\1/p' "$RUN_CFG" | head -1)
MODEL=${BENCH_MODEL:-$CFG_MODEL}

[ -n "$MODEL" ] || skip "no (model ...) in $RUN_CFG"
[ -r "$MODEL" ] || skip "no model at $MODEL
    point it somewhere else with:  make bench BENCH_MODEL=/path/to/model.gguf
    the deterministic scripted-fake benchmark still runs in \`make test\`"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

RUN_USED=$RUN_CFG
SUITE_USED=$SUITE
if [ "$MODEL" != "$CFG_MODEL" ]; then
    RUN_USED=$WORK/run.spg
    SUITE_USED=$WORK/suite.spg
    sed "s|(model \"$CFG_MODEL\")|(model \"$MODEL\")|" "$RUN_CFG" > "$RUN_USED"
    sed "s|(config \"$RUN_CFG\")|(config \"$RUN_USED\")|" "$SUITE" > "$SUITE_USED"
fi

echo "=== real-model benchmark ======================================="
echo "model     : $MODEL"
echo "suite     : $SUITE"
echo "samples   : $SAMPLES    temperature: $TEMP    decoder: constrained"
echo "---------------------------------------------------------------"

# --constrained is the decoder the agent actually ships with; measuring free
# decode would measure a configuration nobody runs (#51). --temperature is what
# makes --samples produce more than one distinct run on a local model — without
# it, greedy plus a fixed seed yields N identical runs.
#
# A failing case is a MEASUREMENT, not a broken build, so the suite's pass/fail
# exit is deliberately not propagated — a baseline that goes red when the model
# is bad at something is useless as a baseline.
#
# A broken HARNESS is a different thing and must not be swallowed. The two look
# identical in the exit code (eval returns 1 for both), so they are told apart
# by the report: a suite that ran to completion always prints its {"suite":...}
# summary line, and one that aborted (model won't load, config unreadable)
# never does.
set +e
"$SPG_BIN" eval "$SUITE_USED" --constrained --samples "$SAMPLES" \
    --temperature "$TEMP" | tee "$WORK/report.jsonl"
set -e

echo "---------------------------------------------------------------"
if ! grep -q '"suite":' "$WORK/report.jsonl"; then
    echo "bench: FAILED — the suite did not run to completion (see the error above)" >&2
    exit 1
fi
echo "bench: complete — verdicts above are measurements, not build failures"
