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
#
# The RAG arm reads MEMORY.md rather than calling `memory list`. That is not a
# shortcut: `memory list` renders the INJECTION index, which is deliberately
# capped at SPG_MEM_INDEX_TOPK (24) lines followed by an "N more" pointer —
# correct for its job, and exactly wrong as a stand-in for an uncapped RAG
# baseline. Measured through it, this arm stopped growing at 24 lessons and sat
# at ~2.2 KB no matter how large the store got, which understated the very cost
# the benchmark exists to show. MEMORY.md is the same index without the cap.
set -eu

SPG_BIN=${SPG_BIN:-build/host-release/bin/geistshell}
[ -x "$SPG_BIN" ] || SPG_BIN=build/host-debug/bin/geistshell
MEM=$(mktemp -d)
trap 'rm -rf "$MEM"' EXIT

bytes() { wc -c | tr -d ' '; }

printf '%-8s %-22s %-22s\n' "lessons" "geistshell_bytes/tick" "full_index_bytes/tick"
K=0
GS_FIRST=
RAG_PREV=
fail=0
# Past 24 on purpose: that is where the injection cap used to flatten this
# curve, so every point beyond it is the regression test for that bug.
for TARGET in 1 4 8 16 32 64; do
    while [ "$K" -lt "$TARGET" ]; do
        K=$((K + 1))
        # distinct slug + a realistic ~120-char directive (reflect-sized)
        printf 'body for lesson %d\n' "$K" | "$SPG_BIN" memory save \
            "lesson-shape-$K" \
            "For a task of shape $K, emit exactly one valid form and finish promptly." \
            --dir "$MEM" >/dev/null
    done
    GS=$("$SPG_BIN" memory directive "lesson-shape-1" --dir "$MEM" | bytes)
    RAG=$(bytes < "$MEM/MEMORY.md")
    printf '%-8s %-22s %-22s\n' "$K" "$GS" "$RAG"

    # The two claims, asserted rather than left to the reader. Printing a table
    # nobody re-derives is how the capped arm went unnoticed for as long as it
    # did: the numbers were right there and simply looked plausible.
    [ -n "$GS_FIRST" ] || GS_FIRST=$GS
    if [ "$GS" -ne "$GS_FIRST" ]; then
        echo "FAIL: geistshell arm is not flat ($GS != $GS_FIRST at $K lessons)"
        fail=1
    fi
    # Each step doubles the lesson count, so an uncapped index must roughly
    # double too. 1.9x leaves room for slug and digit widths; the cap produced
    # 1.54x, which this catches.
    if [ -n "$RAG_PREV" ] && [ $((RAG * 10)) -lt $((RAG_PREV * 19)) ]; then
        echo "FAIL: full index grew only ${RAG}/${RAG_PREV} over a doubling;" \
             "expected at least 1.9x — is it being read through a capped view?"
        fail=1
    fi
    [ "$TARGET" -eq 1 ] || RAG_PREV=$RAG
done

[ "$fail" -eq 0 ] || exit 1
