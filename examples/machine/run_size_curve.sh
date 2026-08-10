#!/bin/sh
# Model size curve (roadmap phase 12, #72).
#
# The same suite, the same prompt, the same context, the same seed — only the
# model changes. That is the experiment; a curve where the prompt also moved
# would measure two things.
#
# Models are named on the command line as SIZE_LABEL=PATH. A tier nobody has a
# model for is reported as "not tested" rather than left out, because a gap in
# a curve is information and a missing row looks like a curve without a gap.
set -eu

SPG_BIN=${SPG_BIN:-build/host-release/bin/geistshell}
SUITE=${SUITE:-examples/eval/machine/diagnosis_model.spg}
RUNCFG=${RUNCFG:-examples/eval/machine/run_local.spg}
OUT=${OUT:-build/size-curve}
SAMPLES=${SAMPLES:-1}

# --out before the model list, because writing OUT=dir among the specs reads as
# a model named OUT and produces a "not tested" row for a directory. Found by
# doing exactly that.
while [ $# -gt 0 ]; do
    case "$1" in
        --out) OUT=$2; shift 2 ;;
        --samples) SAMPLES=$2; shift 2 ;;
        --suite) SUITE=$2; shift 2 ;;
        *) break ;;
    esac
done

mkdir -p "$OUT"
: >"$OUT/curve.jsonl"

for spec in "$@"; do
    label=${spec%%=*}
    path=${spec#*=}
    case "$spec" in
        *=*) ;;
        *) echo "expected LABEL=PATH, got: $spec" >&2; exit 2 ;;
    esac
    if [ ! -f "$path" ]; then
        printf '{"label":"%s","status":"not_tested","why":"no model file"}\n' \
            "$label" >>"$OUT/curve.jsonl"
        continue
    fi
    size_bytes=$(wc -c <"$path")
    sed "s|(model \"[^\"]*\")|(model \"$path\")|" "$RUNCFG" >"$OUT/run-$label.spg"
    sed "s|(config \"[^\"]*\")|(config \"$OUT/run-$label.spg\")|" "$SUITE" \
        >"$OUT/suite-$label.spg"

    "$SPG_BIN" eval "$OUT/suite-$label.spg" --samples "$SAMPLES" --timing \
        --constrained >"$OUT/cases-$label.jsonl" 2>"$OUT/err-$label.log" || true

    python3 - "$OUT/cases-$label.jsonl" "$label" "$size_bytes" \
        "$OUT/curve.jsonl" <<'PY'
import json, subprocess, sys

cases_path, label, size_bytes, out = sys.argv[1:5]
rows = []
try:
    with open(cases_path, encoding="utf-8") as f:
        rows = [json.loads(l) for l in f if l.strip()]
except (OSError, json.JSONDecodeError):
    rows = []
summary = next((r for r in rows if "suite" in r), None)
cases = [r for r in rows if "name" in r]
if not cases:
    record = {"label": label, "status": "failed",
              "why": "the model produced no scoreable run"}
else:
    diag = (summary or {}).get("diagnosis", {})
    modes = json.loads(subprocess.run(
        ["python3", "examples/machine/categorise_failures.py", cases_path],
        capture_output=True, text=True, check=True).stdout)
    record = {
        "label": label,
        "status": "tested",
        "size_bytes": int(size_bytes),
        "size_gb": round(int(size_bytes) / 1073741824, 2),
        "known": diag.get("known"),
        "known_runs": diag.get("known_runs"),
        "heldout": diag.get("heldout"),
        "heldout_runs": diag.get("heldout_runs"),
        "parsed": (summary or {}).get("parsed"),
        "gated": (summary or {}).get("gated"),
        "unsafe": diag.get("action_proposed"),
        "latency_ms": (summary or {}).get("latency_ms"),
        "failure_modes": modes["modes"],
        "examples": modes["examples"],
    }
with open(out, "a", encoding="utf-8") as f:
    f.write(json.dumps(record) + "\n")
PY
done

python3 - "$OUT/curve.jsonl" <<'PY'
import json, sys

rows = [json.loads(l) for l in open(sys.argv[1], encoding="utf-8")]
print(f"{'model':<22} {'size':>7} {'known':>7} {'held-out':>9} "
      f"{'parse':>7} {'unsafe':>7} {'dominant failure':<24}")
for r in rows:
    if r.get("status") != "tested":
        print(f"{r['label']:<22} {'–':>7} {'not tested':>7} "
              f"{'':>9} {'':>7} {'':>7} {r.get('why', '')}")
        continue
    modes = r.get("failure_modes") or {}
    dominant = max(modes, key=modes.get) if modes else "–"
    n = modes.get(dominant, 0)
    print(f"{r['label']:<22} {str(r['size_gb']) + ' GB':>7} "
          f"{f'{r['known']}/{r['known_runs']}':>7} "
          f"{f'{r['heldout']}/{r['heldout_runs']}':>9} "
          f"{str(r['parsed']):>7} {str(r['unsafe']):>7} "
          f"{f'{dominant} ({n})':<24}")
PY
