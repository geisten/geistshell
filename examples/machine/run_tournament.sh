#!/bin/sh
# Model tournament (roadmap phase 10, #70).
#
# Runs the SAME machine scenarios through every model and writes one row per
# model. No new model framework: every entrant goes through the existing
# adapter and the existing eval harness, or the comparison would be between two
# harnesses rather than two models.
#
#   run_tournament.sh [--samples N] [--models "fake geist:/path/to.gguf remote"]
#                     [--out-dir build]
#
# A model that cannot be reached produces an error row. It is never counted as
# a pass and never silently skipped — a table with a missing entrant invites
# the reader to assume it was fine.
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}
SAMPLES=1
OUT_DIR=build
MODELS="fake"
# Two suites, because a scripted case and a model case are different files:
# the first pins the ground truth, the second declares (model "geist") per case
# so a real model actually runs. Pointing every entrant at the scripted suite
# is how you get a table where a 3 GB model scores 6/6 in 0 ms — which is what
# the first version of this script did, and the table looked perfectly fine.
SUITE=examples/eval/machine/diagnosis.spg
MODEL_SUITE=examples/eval/machine/diagnosis_model.spg

while [ $# -gt 0 ]; do
    case "$1" in
        --samples) SAMPLES=$2; shift 2 ;;
        --models) MODELS=$2; shift 2 ;;
        --out-dir) OUT_DIR=$2; shift 2 ;;
        --suite) SUITE=$2; shift 2 ;;
        --model-suite) MODEL_SUITE=$2; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

mkdir -p "$OUT_DIR"
JSONL="$OUT_DIR/machine-benchmark.jsonl"
MD="$OUT_DIR/machine-benchmark.md"
: >"$JSONL"

for entry in $MODELS; do
    kind=${entry%%:*}
    path=${entry#*:}
    [ "$path" = "$entry" ] && path=""

    RAW="$OUT_DIR/tournament-$kind.jsonl"
    STATUS=0
    SIZE=0
    [ -n "$path" ] && [ -f "$path" ] && SIZE=$(wc -c <"$path" | tr -d ' ')

    case "$kind" in
        fake)
            # The scripted baseline. Not a model: the control that shows the
            # harness itself scores correctly.
            "$SPG_BIN" eval "$SUITE" --samples "$SAMPLES" --timing >"$RAW" 2>/dev/null \
                || STATUS=$?
            ;;
        geist)
            if [ -z "$path" ] || [ ! -f "$path" ]; then
                echo "tournament: no model file for geist ($path)" >&2
                STATUS=127
                : >"$RAW"
            else
                # The model path lives in the run config, not the environment,
                # so an entrant needs its own config and a suite pointing at
                # it. Generated rather than checked in: the path is per host.
                RUNCFG="$OUT_DIR/tournament-run-$kind.spg"
                SUITECFG="$OUT_DIR/tournament-suite-$kind.spg"
                BASECFG=$(sed -n 's/.*(config "\([^"]*\)").*/\1/p' \
                          "$MODEL_SUITE" | head -1)
                # Only the model line is replaced; policy, budgets and journal
                # stay exactly as configured.
                sed "s|(model \"[^\"]*\")|(model \"$path\")|" "$BASECFG" \
                    >"$RUNCFG"
                sed "s|(config \"[^\"]*\")|(config \"$RUNCFG\")|" \
                    "$MODEL_SUITE" >"$SUITECFG"
                # A model entrant whose suite has no model cases would quietly
                # run the scripted answers and produce a table that looks
                # right. Refuse instead: a wrong number is worse than none.
                if ! grep -q '(model "geist")' "$SUITECFG"; then
                    echo "tournament: $MODEL_SUITE has no (model \"geist\") cases" >&2
                    STATUS=2
                    : >"$RAW"
                else
                    "$SPG_BIN" eval "$SUITECFG" --samples "$SAMPLES" \
                        --constrained --timing >"$RAW" 2>/dev/null || STATUS=$?
                fi
            fi
            ;;
        remote)
            if [ -z "${GEISTSHELL_API_URL:-}" ]; then
                echo "tournament: GEISTSHELL_API_URL unset, remote not run" >&2
                STATUS=127
                : >"$RAW"
            else
                "$SPG_BIN" eval "$SUITE" --samples "$SAMPLES" --timing \
                    --remote-url "$GEISTSHELL_API_URL" >"$RAW" 2>/dev/null \
                    || STATUS=$?
            fi
            ;;
        *)
            echo "tournament: unknown model kind $kind" >&2
            STATUS=2
            : >"$RAW"
            ;;
    esac

    python3 - "$RAW" "$kind" "$path" "$SIZE" "$STATUS" "$SAMPLES" "$JSONL" <<'PY'
import json, sys

raw, kind, path, size, status, samples, out = sys.argv[1:8]
rows = []
try:
    with open(raw, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line:
                rows.append(json.loads(line))
except (OSError, json.JSONDecodeError):
    rows = []

cases = [r for r in rows if "name" in r]
summary = next((r for r in rows if "suite" in r), None)

record = {
    "model": kind,
    "model_path": path or None,
    "model_size_bytes": int(size) or None,
    "samples": int(samples),
    "status": int(status),
}

if summary is None:
    # No usable output. Reported as an error row rather than omitted: a table
    # with a missing entrant reads as "it was fine".
    record.update({"available": False, "known": None, "heldout": None,
                   "unsafe": None, "reject_rate": None, "deny_rate": None,
                   "mean_steps": None, "p95_steps": None,
                   "latency_ms": None, "peak_rss_kb": None})
else:
    diag = summary.get("diagnosis", {})
    total = summary.get("total", 0) or 0
    parsed = summary.get("parsed", 0) or 0
    gated = summary.get("gated", 0) or 0
    steps = sorted(c.get("steps", 0) for c in cases)

    def p95(values):
        if not values:
            return None
        # Nearest-rank. With nine cases this is the second-worst, and saying so
        # matters more than the number: a p95 over a handful of samples is a
        # description, not a statistic.
        import math
        idx = max(0, math.ceil(0.95 * len(values)) - 1)
        return values[idx]

    record.update({
        "available": True,
        # Known and held-out never averaged: the split is the point.
        "known": diag.get("known"),
        "known_runs": diag.get("known_runs"),
        "heldout": diag.get("heldout"),
        "heldout_runs": diag.get("heldout_runs"),
        "unsafe": diag.get("action_proposed"),
        "hallucinated": diag.get("hallucinated"),
        # A recommendation that did not parse, and one that parsed but was
        # denied, are different failures and stay apart.
        "reject_rate": round((total - parsed) / total, 4) if total else None,
        "deny_rate": round((parsed - gated) / total, 4) if total else None,
        "mean_steps": round(sum(steps) / len(steps), 2) if steps else None,
        "p95_steps": p95(steps),
        "latency_ms": summary.get("latency_ms"),
        "peak_rss_kb": summary.get("peak_rss_kb"),
    })

with open(out, "a", encoding="utf-8") as f:
    f.write(json.dumps(record) + "\n")
PY
done

python3 - "$JSONL" "$MD" "$SUITE" "$MODEL_SUITE" <<'PY'
import json, sys

jsonl, md, suite, model_suite = sys.argv[1:5]
rows = [json.loads(l) for l in open(jsonl, encoding="utf-8")]

def cell(value, suffix=""):
    return "–" if value is None else f"{value}{suffix}"

def size_mb(value):
    return "–" if not value else f"{value / (1024 * 1024):.0f} MB"

lines = [
    "# Machine Benchmark",
    "",
    f"Scripted control: `{suite}`. Model entrants: `{model_suite}`.",
    "Generated by `examples/machine/run_tournament.sh`.",
    "",
    "Known and held-out are reported apart and never averaged. A model that",
    "could not be run appears as a row with no numbers rather than not at all.",
    "",
    "| Model | Size | Known | Held-out | Unsafe | Reject | Steps | p95 | Latency | Peak RSS |",
    "|---|---|---|---|---|---|---|---|---|---|",
]
for r in rows:
    if not r.get("available"):
        lines.append(
            f"| {r['model']} | {size_mb(r['model_size_bytes'])} | "
            f"not run (status {r['status']}) | | | | | | | |")
        continue
    known = f"{r['known']}/{r['known_runs']}" if r.get("known_runs") else "–"
    held = f"{r['heldout']}/{r['heldout_runs']}" if r.get("heldout_runs") else "–"
    lines.append(
        f"| {r['model']} | {size_mb(r['model_size_bytes'])} | {known} | {held} "
        f"| {cell(r['unsafe'])} | {cell(r['reject_rate'])} "
        f"| {cell(r['mean_steps'])} | {cell(r['p95_steps'])} "
        f"| {cell(r['latency_ms'], ' ms')} | {cell(r['peak_rss_kb'], ' kB')} |")

lines += [
    "",
    "Latency is the whole suite on this host, not per inference — the harness",
    "times cases, and a per-token number would need instrumentation inside the",
    "adapter. Peak RSS is the harness process, so it includes the model.",
    "",
]
open(md, "w", encoding="utf-8").write("\n".join(lines))
print(f"wrote {jsonl} and {md}")
PY
