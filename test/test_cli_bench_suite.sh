#!/bin/sh
# System test: the real-model baseline suite is STRUCTURALLY sound
# (geistshell#53).
#
# The suite itself cannot run here — it needs a GGUF and minutes, which is why
# it lives behind `make bench`. But almost everything that can be wrong with it
# is wrong without a model: a fixture path that does not exist, a case with no
# goal, a hold-out that quietly duplicates its train twin, a policy that masks
# the capability slot down to one choice so it measures nothing.
#
# Those are exactly the defects that would be discovered late — after a
# multi-minute run on a Pi, or worse, not at all, because a suite that silently
# measures the wrong thing still prints numbers.
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}
B=examples/eval/bench
TRAIN=$B/model_train.spg
HOLDOUT=$B/model_holdout.spg
POLICY=$B/model_policy.spg
RUN=$B/model_run.spg

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

die() { echo "FAIL: $*" >&2; exit 1; }

# case names, in file order
names() { grep -o '(name "[^"]*")' "$1" | sed 's/(name "//;s/")//'; }
# values of a single-string field, in file order
field() { grep -o "($2 \"[^\"]*\")" "$1" | sed "s/($2 \"//;s/\")//"; }

# ---- 1. the shared configs load -------------------------------------------
"$SPG_BIN" policy-check "$POLICY" | grep -q "valid=true" ||
    die "$POLICY does not validate"
[ -r "$RUN" ] || die "missing run config $RUN"

# ---- 2. the capability slot is actually a choice ---------------------------
# With one capability per kind the constrained decoder's capability mask has a
# single candidate, so cap-distractor would measure nothing at all. This is the
# assertion that keeps that from happening silently.
SHELL_CAPS=$(grep -c '(kind local_shell)' "$POLICY")
[ "$SHELL_CAPS" -ge 2 ] ||
    die "policy has $SHELL_CAPS local_shell capabilities; the distractor cases need >= 2"
grep -q '(name auth_probe.ssh_publickey_single) (kind ssh_auth_probe) (enabled false)' "$POLICY" ||
    die "denied-probe needs the ssh capability present but disabled"

# ---- 3. every case is complete -------------------------------------------
for S in "$TRAIN" "$HOLDOUT"; do
    N=$(names "$S" | wc -l | tr -d ' ')
    [ "$N" = "7" ] || die "$S has $N cases, expected 7"

    # one goal, one model, one fixture, one expect per case
    for f in goal fixture; do
        C=$(field "$S" "$f" | wc -l | tr -d ' ')
        [ "$C" = "7" ] || die "$S has $C ($f ...) entries, expected 7"
    done
    C=$(grep -c '(model "geist")' "$S")
    [ "$C" = "7" ] || die "$S has $C (model \"geist\") entries, expected 7"
    C=$(grep -c '(expect ' "$S")
    [ "$C" = "7" ] || die "$S has $C (expect ...) entries, expected 7"

    # every fixture directory exists and is non-empty.
    # NB: fed by redirect, not by a pipe — a pipe puts the loop in a subshell
    # where `die` would exit only the subshell and the test would pass anyway.
    field "$S" fixture > "$T/fixtures.txt"
    while read -r d; do
        [ -d "$d" ] || die "$S references a missing fixture: $d"
        [ -n "$(ls -A "$d")" ] || die "fixture $d is empty"
    done < "$T/fixtures.txt"

    # a goal that names the action kind or the capability would be giving away
    # the very decision the suite exists to measure
    field "$S" goal > "$T/goals.txt"
    while read -r g; do
        case "$g" in
            *local_shell*|*memory_save*|*memory_read*|*simulator*|*ssh_auth_probe*|\
            *fs.read*|*build.run*|*mem.rw*|*sim.act*)
                die "a goal names the action kind or capability: $g" ;;
        esac
    done < "$T/goals.txt"

    # case names must be sandbox-safe (spg_fixture_sample_dir rejects the rest)
    names "$S" > "$T/names.txt"
    while read -r n; do
        case "$n" in
            *[!A-Za-z0-9._-]*) die "case name is not sandbox-safe: $n" ;;
        esac
    done < "$T/names.txt"
done

# ---- 4. train and hold-out are paired, one for one ------------------------
PAIRED=$(names "$TRAIN" | sed 's/$/-h/')
ACTUAL=$(names "$HOLDOUT")
[ "$PAIRED" = "$ACTUAL" ] || {
    echo "hold-out cases must be <train-name>-h, in the same order" >&2
    printf 'expected:\n%s\ngot:\n%s\n' "$PAIRED" "$ACTUAL" >&2
    exit 1
}

# ---- 5. ... and structurally DIFFERENT ------------------------------------
# A hold-out that reuses the train fixture or expects the same string measures
# the train split again, and "it generalises" becomes an assertion rather than
# a result.
field "$TRAIN" fixture | sort > "$T/train-fixtures.txt"
field "$HOLDOUT" fixture | sort > "$T/holdout-fixtures.txt"
SHARED=$(comm -12 "$T/train-fixtures.txt" "$T/holdout-fixtures.txt")
[ -z "$SHARED" ] || die "train and hold-out share a fixture: $SHARED"

field "$TRAIN" observation | sort > "$T/train-obs.txt"
field "$HOLDOUT" observation | sort > "$T/holdout-obs.txt"
SHARED_OBS=$(comm -12 "$T/train-obs.txt" "$T/holdout-obs.txt")
[ -z "$SHARED_OBS" ] || die "train and hold-out expect the same observation: $SHARED_OBS"

# ---- 6. the bench target points at the train suite ------------------------
grep -q 'BENCH_SUITE:-examples/eval/bench/model_train.spg' \
    "$B/model_bench.sh" || die "model_bench.sh does not default to the train suite"

echo "test_cli_bench_suite: PASS"
