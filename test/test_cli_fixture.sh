#!/bin/sh
# System test: per-sample fixture isolation (geistshell#52).
#
# The defect this guards: a stateful case mutates what it runs against, so a
# second sample finds its own previous mutation already there and passes
# WITHOUT performing the action under test. With --samples N that is N-1
# fabricated successes — worse than no measurement, because it looks like one.
#
# Everything here runs on scripted fakes, so it needs no GGUF and no network.
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
SUITE=examples/eval/fixture_suite.spg
FIXTURE_WORKDIR=examples/eval/fixtures/workdir
FIXTURE_MEM=examples/eval/fixtures/memory

# ---- 1. a stale sandbox from an earlier run must not survive into this one ---
mkdir -p build/eval/iso-write-0
echo "left over from a previous run" > build/eval/iso-write-0/stale.md

OUT=$("$SPG_BIN" eval "$SUITE" --samples 3)
printf '%s\n' "$OUT"

printf '%s\n' "$OUT" | grep -q '"name":"iso-write","outcome":"pass".*"runs":3,"passed":3'
printf '%s\n' "$OUT" | grep -q '"name":"iso-probe","outcome":"pass".*"runs":3,"passed":3'
printf '%s\n' "$OUT" | grep -q '"name":"iso-mem","outcome":"pass".*"runs":3,"passed":3'
printf '%s\n' "$OUT" | grep -q '"total":9,"passed":9'

if [ -e build/eval/iso-write-0/stale.md ]; then
    echo "FAIL: the sandbox was not reset — a previous run's file survived" >&2
    exit 1
fi

# ---- 2. every sample got its OWN sandbox, and every sample really ran -------
for s in 0 1 2; do
    [ -f "build/eval/iso-write-$s/mutated.md" ] || {
        echo "FAIL: sample $s did not run in its own sandbox" >&2
        exit 1
    }
    # the fixture is restored for each sample, not carried over
    [ -f "build/eval/iso-write-$s/report.md" ] || {
        echo "FAIL: sample $s did not get a pristine fixture" >&2
        exit 1
    }
done

# ---- 3. no leak across cases ------------------------------------------------
if [ -e build/eval/iso-probe-0/mutated.md ]; then
    echo "FAIL: one case saw another case's mutation" >&2
    exit 1
fi

# ---- 4. the fixture SOURCE is never written ---------------------------------
# If this fails the suite is not reproducible at all: the next run starts from
# a fixture the previous run edited, and the checked-in tree is dirty.
if [ -e "$FIXTURE_WORKDIR/mutated.md" ]; then
    echo "FAIL: the run wrote into the fixture source directory" >&2
    exit 1
fi
FIXTURE_FILES=$(ls "$FIXTURE_WORKDIR")
[ "$FIXTURE_FILES" = "report.md" ] || {
    echo "FAIL: fixture source contents changed: $FIXTURE_FILES" >&2
    exit 1
}

# ---- 5. a fixture may seed the mind-palace ----------------------------------
# iso-mem passes only because memory_read found the seeded slug, so the pass
# above already proves the store was opened on the sandbox copy. Check the
# source is untouched and nothing accumulated in the sandbox.
[ -f "$FIXTURE_MEM/mem/staging-port.md" ] || {
    echo "FAIL: seeded memory missing from the fixture source" >&2
    exit 1
}
for s in 0 1 2; do
    # MEMORY.md is a generated index and may or may not exist; only real
    # memories are counted.
    N=$(ls "build/eval/iso-mem-$s/mem" | grep -cv '^MEMORY\.md$')
    [ "$N" = "1" ] || {
        echo "FAIL: sample $s mind-palace has $N memories, expected 1" >&2
        ls "build/eval/iso-mem-$s/mem" >&2
        exit 1
    }
done

# ---- 6. the whole suite is reproducible -------------------------------------
OUT2=$("$SPG_BIN" eval "$SUITE" --samples 3)
[ "$OUT" = "$OUT2" ] || {
    echo "FAIL: a second identical run produced a different report" >&2
    printf 'first:\n%s\nsecond:\n%s\n' "$OUT" "$OUT2" >&2
    exit 1
}

# ---- 7. a missing fixture is a reported run error, not a crash --------------
cat > "$T/missing.spg" <<EOF
(eval_suite
 (config "examples/eval/fixture_run.spg")
 (case (name "no-fixture") (script "examples/eval/fixture_probe.txt")
       (fixture "examples/eval/fixtures/does-not-exist") (allow_exec true)
       (max_steps 4)
       (expect (termination finished))))
EOF
if "$SPG_BIN" eval "$T/missing.spg" > "$T/missing.out" 2>&1; then
    echo "FAIL: a missing fixture should fail the suite" >&2
    cat "$T/missing.out" >&2
    exit 1
fi
grep -q '"name":"no-fixture","outcome":"fail_run_error"' "$T/missing.out" || {
    echo "FAIL: a missing fixture should be reported as a run error" >&2
    cat "$T/missing.out" >&2
    exit 1
}

# ---- 8. a case name that could steer a path is refused ----------------------
# spg_fixture_sample_dir rejects the name, so the sample cannot run; the suite
# must report that rather than deleting something outside build/eval.
cat > "$T/evil.spg" <<EOF
(eval_suite
 (config "examples/eval/fixture_run.spg")
 (case (name "../escape") (script "examples/eval/fixture_probe.txt")
       (fixture "examples/eval/fixtures/workdir") (allow_exec true)
       (max_steps 4)
       (expect (termination finished))))
EOF
if "$SPG_BIN" eval "$T/evil.spg" > "$T/evil.out" 2>&1; then
    echo "FAIL: a path-steering case name should fail the suite" >&2
    cat "$T/evil.out" >&2
    exit 1
fi
grep -q '"outcome":"fail_run_error"' "$T/evil.out" || {
    echo "FAIL: a path-steering case name should be a run error" >&2
    cat "$T/evil.out" >&2
    exit 1
}
[ -d build/eval ] || {
    echo "FAIL: build/eval disappeared — a delete escaped the sandbox root" >&2
    exit 1
}

# ---- 9. `make bench` skips without a model, but not when it is broken -------
BENCH=examples/eval/bench/model_bench.sh

# no model -> skipped, exit 0. This is the property that keeps the target alive:
# a benchmark that reddens the build on a fresh checkout gets commented out.
BENCH_MODEL="$T/definitely-absent.gguf" SPG_BIN="$SPG_BIN" sh "$BENCH" \
    > "$T/bench-skip.out" 2>&1 || {
    echo "FAIL: bench must exit 0 when no model is present" >&2
    cat "$T/bench-skip.out" >&2
    exit 1
}
grep -q '^bench: skipped' "$T/bench-skip.out" || {
    echo "FAIL: bench must say it skipped" >&2
    cat "$T/bench-skip.out" >&2
    exit 1
}

# a model that exists but will not load is a BROKEN HARNESS, not a measurement,
# and must be non-zero — otherwise a corrupt GGUF reports a successful bench.
echo "not a gguf" > "$T/corrupt.gguf"
if BENCH_MODEL="$T/corrupt.gguf" SPG_BIN="$SPG_BIN" sh "$BENCH" \
        > "$T/bench-broken.out" 2>&1; then
    echo "FAIL: bench must fail when the suite cannot run at all" >&2
    cat "$T/bench-broken.out" >&2
    exit 1
fi
grep -q 'did not run to completion' "$T/bench-broken.out" || {
    echo "FAIL: bench must say why it failed" >&2
    cat "$T/bench-broken.out" >&2
    exit 1
}

# a suite that RUNS is exit 0 even when cases fail — a baseline that goes red
# when the model is bad at something is useless as a baseline. Driven here with
# scripted fakes so it needs no GGUF.
BENCH_SUITE=examples/eval/fixture_suite.spg BENCH_RUN=examples/eval/fixture_run.spg \
    BENCH_MODEL="$T/corrupt.gguf" SPG_BIN="$SPG_BIN" sh "$BENCH" \
    > "$T/bench-ran.out" 2>&1 || {
    echo "FAIL: bench must exit 0 when the suite runs to completion" >&2
    cat "$T/bench-ran.out" >&2
    exit 1
}
grep -q '^bench: complete' "$T/bench-ran.out" || {
    echo "FAIL: bench must report completion" >&2
    exit 1
}

# ---- 10. cases WITHOUT a fixture keep the historical behaviour --------------
# The shipped suite declares none, so its report must be untouched by all of
# the above.
"$SPG_BIN" eval examples/eval/suite.spg | \
    grep -q '"suite":"examples/eval/suite.spg","total":3,"passed":3'

echo "test_cli_fixture: PASS"
