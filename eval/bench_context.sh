#!/bin/sh
# Benchmark (geistshell#25): the context cost of learning, per tick, as the
# lesson set grows. The load-bearing half of the small-seq-len claim, measured
# model-free and deterministically.
#
#   geistshell arm  = one slug's directive (P6 auto-injection): must stay flat
#   full-index RAG  = the whole mind-palace index (the large-model path): grows
#
# It does NOT measure task-success lift — that needs a model-completable corpus
# and real inference (the remaining half of #25). This proves only the
# denominator: geistshell's learning is context-invariant, RAG's is not.
set -eu

SPG_BIN=${SPG_BIN:-build/host-release/bin/geistshell}
[ -x "$SPG_BIN" ] || SPG_BIN=build/host-debug/bin/geistshell
MEM=$(mktemp -d)
trap 'rm -rf "$MEM"' EXIT

bytes() { wc -c | tr -d ' '; }

printf '%-8s %-22s %-22s\n' "lessons" "geistshell_bytes/tick" "full_index_bytes/tick"
K=0
for TARGET in 1 4 8 16 32; do
    while [ "$K" -lt "$TARGET" ]; do
        K=$((K + 1))
        # distinct slug + a realistic ~120-char directive (reflect-sized)
        printf 'body for lesson %d\n' "$K" | "$SPG_BIN" memory save \
            "lesson-shape-$K" \
            "For a task of shape $K, emit exactly one valid form and finish promptly." \
            --dir "$MEM" >/dev/null
    done
    GS=$("$SPG_BIN" memory directive "lesson-shape-1" --dir "$MEM" | bytes)
    RAG=$("$SPG_BIN" memory list --dir "$MEM" | bytes)
    printf '%-8s %-22s %-22s\n' "$K" "$GS" "$RAG"
done
