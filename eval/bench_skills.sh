#!/bin/sh
# Before/after harness for success-side skill abstraction (geistshell#26).
# Ten tasks that each CREATE AND RUN a real script (local_shell), share one
# capability shape, and carry a verifiable (expect). A fake model stands in for
# the recommendation (Gemma cannot emit the DSL untrained — the #25 corpus
# prerequisite); the shell runs for real.
#
# BEFORE: no distillation, no skill accumulates.
# AFTER:  `distill` after each pass mints skill-<shape> from the successful
#         trajectory; present in the store, it is injected into every later
#         same-shape task via the mind-palace index the agent already renders.
set -eu

SPG=${SPG_BIN:-build/host-debug/bin/geistshell}
SKILL_SLUG="skill-finish-local-shell-build-run" # the shape of a shell+finish run
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

# commands use NO inner double-quotes (single quotes are literal inside the
# s-expr string), so each is a real, verifiable script with clean quoting.
i=0; add() { i=$((i+1)); eval "G$i=\$1; C$i=\$2; E$i=\$3"; }
add "print a greeting"       'echo HELLO-WORLD'                        HELLO-WORLD
add "multiply two numbers"   'expr 6 \* 7'                             42
add "count words"            'echo a b c d | wc -w'                    4
add "sort three numbers"     "printf '3\n1\n2\n' | sort | tr '\n' ' '" "1 2 3"
add "sum a bash range"       'echo $((10+20+30))'                      60
add "reverse a string"       'echo abc | rev'                         cba
add "grep a pattern"         "printf 'foo\nbar\n' | grep bar"         bar
add "generate a sequence"    "seq 1 5 | tr '\n' ' '"                   "1 2 3 4 5"
add "uppercase a word"       'echo hello | tr a-z A-Z'                 HELLO
add "write then read a file" "echo made-a-file > $T/f && cat $T/f"    made-a-file
N=$i

run_pass() { # $1 = memory dir, $2 = distill|none ; prints table, returns via echo
    MEM=$1; MODE=$2; pass=0; inj_total=0; k=0
    while [ "$k" -lt "$N" ]; do
        k=$((k+1)); eval "goal=\$G$k; cmd=\$C$k; exp=\$E$k"
        J="$T/j_${MODE}_$k.sgj"
        printf '(run\n (model "fake.gguf")\n (policy "examples/policy.spg")\n (scenario "examples/scenario.spg")\n (corpus "examples/corpus.spg")\n (journal "%s")\n (seed 42)\n (budgets (inference_steps 4) (tokens 256) (shell_actions 1) (sim_actions 8) (wall_ms 30000) (journal_bytes 1048576) (risk_bp 10000))\n (expect "%s"))\n' "$J" "$exp" > "$T/run.spg"
        printf '(recommend (kind local_shell) (capability "build.run") (cost 1) (uses_network false) (confidence_bp 7000) (reason "task") (command "%s"))\n(recommend (kind finish) (reason "done"))\n' "$cmd" > "$T/s.txt"

        "$SPG" memory directive "$SKILL_SLUG" --dir "$MEM" >/dev/null 2>&1 && inj=yes || inj=no
        [ "$inj" = yes ] && inj_total=$((inj_total+1))
        out=$("$SPG" agent --config "$T/run.spg" --fake-script "$T/s.txt" --memory-dir "$MEM" --allow-exec --max-steps 4 2>&1 || true)
        echo "$out" | grep -q "verdict=pass" && { s=pass; pass=$((pass+1)); } || s=FAIL
        [ "$MODE" = distill ] && [ "$s" = pass ] && "$SPG" distill "$J" --memory-dir "$MEM" >/dev/null 2>&1 || true
        printf '  %2d  %-22s %-6s skill_injected=%s\n' "$k" "$goal" "$s" "$inj"
    done
    printf '  => %d/%d solved, skill injected into %d/%d tasks\n' "$pass" "$N" "$inj_total" "$N"
}

echo "=== BEFORE #26 (no distillation) ==="
M1=$(mktemp -d); run_pass "$M1" none; rm -rf "$M1"
echo; echo "=== AFTER #26 (distil after each pass) ==="
M2=$(mktemp -d); run_pass "$M2" distill; rm -rf "$M2"

echo
echo "Reading: skill injection goes 0/N -> ~N/N once distillation is on — the"
echo "feature works. Task-success is identical across before/after because a"
echo "FAKE model ignores the injected skill; whether the skill LIFTS success"
echo "needs a real model reacting to it (the #25 numerator, on a model-"
echo "completable corpus). This harness proves accumulation + injection, not lift."
