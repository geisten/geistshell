#!/bin/sh
# Context ablation (roadmap phase 11, #71).
#
# Runs the SAME suite, the SAME model and the SAME seed with parts of the
# snapshot withheld, to find out which of them the model actually uses.
#
# Everything except the mask is held constant — that is the experiment. A
# variant that also changed the sampling parameters would measure two things.
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}
SUITE=examples/eval/machine/diagnosis.spg
OUT=build/ablation.jsonl
SAMPLES=1
EXTRA=""

while [ $# -gt 0 ]; do
    case "$1" in
        --suite) SUITE=$2; shift 2 ;;
        --out) OUT=$2; shift 2 ;;
        --samples) SAMPLES=$2; shift 2 ;;
        --constrained) EXTRA="$EXTRA --constrained"; shift ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

mkdir -p "$(dirname "$OUT")"
: >"$OUT"

# The variants from the ticket, plus the floor. `full` is the control; without
# it the other rows have nothing to be smaller or worse than.
#
# `bare` is the sanity floor and the most important row in the table: if a model
# scores well with almost no context, the scenarios were too easy and every
# other number is meaningless.
run_variant() {
    name=$1
    mask=$2
    raw="$(dirname "$OUT")/ablation-$name.jsonl"
    if [ -z "$mask" ]; then
        # shellcheck disable=SC2086
        "$SPG_BIN" eval "$SUITE" --samples "$SAMPLES" --timing $EXTRA \
            >"$raw" 2>/dev/null || true
    else
        # shellcheck disable=SC2086
        "$SPG_BIN" eval "$SUITE" --samples "$SAMPLES" --timing --ablate "$mask" \
            $EXTRA >"$raw" 2>/dev/null || true
    fi
    python3 - "$raw" "$name" "$mask" "$OUT" <<'PY'
import json, sys

raw, name, mask, out = sys.argv[1:5]
rows = []
try:
    with open(raw, encoding="utf-8") as f:
        rows = [json.loads(l) for l in f if l.strip()]
except (OSError, json.JSONDecodeError):
    rows = []
summary = next((r for r in rows if "suite" in r), None)
cases = [r for r in rows if "name" in r]
diag = (summary or {}).get("diagnosis", {})
ctx = [c.get("context_bytes", 0) for c in cases if c.get("context_bytes")]
record = {
    "variant": name,
    "ablate": mask or "none",
    "known": diag.get("known"),
    "known_runs": diag.get("known_runs"),
    "heldout": diag.get("heldout"),
    "heldout_runs": diag.get("heldout_runs"),
    # Action safety: a diagnosis-only suite that gets an action proposed is the
    # unsafe case, and a smaller context must not make it more likely.
    "unsafe": diag.get("action_proposed"),
    "mean_steps": round(sum(c.get("steps", 0) for c in cases) / len(cases), 2)
                  if cases else None,
    "context_bytes_mean": round(sum(ctx) / len(ctx)) if ctx else None,
    "latency_ms": (summary or {}).get("latency_ms"),
}
with open(out, "a", encoding="utf-8") as f:
    f.write(json.dumps(record) + "\n")
PY
}

run_variant full ""
run_variant no_role role
run_variant no_temperature temperature
run_variant no_frequency frequency
run_variant no_memory memory
run_variant no_processes processes
run_variant minimal "frequency,memory,load"
run_variant bare "role,temperature,frequency,memory,processes,load"

python3 - "$OUT" <<'PY'
import json, sys

rows = [json.loads(l) for l in open(sys.argv[1], encoding="utf-8")]
print(f"{'variant':<16} {'known':>7} {'held-out':>9} {'unsafe':>7} "
      f"{'ctx bytes':>10} {'latency':>9}")
for r in rows:
    known = f"{r['known']}/{r['known_runs']}" if r.get("known_runs") else "–"
    held = f"{r['heldout']}/{r['heldout_runs']}" if r.get("heldout_runs") else "–"
    print(f"{r['variant']:<16} {known:>7} {held:>9} "
          f"{str(r['unsafe']):>7} {str(r['context_bytes_mean']):>10} "
          f"{str(r['latency_ms']) + ' ms':>9}")
PY
