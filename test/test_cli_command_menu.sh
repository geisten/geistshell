#!/bin/sh
# System test: the command menu reaches the model, and reaches NOTHING ELSE
# (geistshell#56).
#
# The menu carried `summary` and `common_flags` — fields only a model can use —
# and no model ever saw them. It read as a security layer that does nothing;
# it was a tool menu that was never plugged in.
#
# Half of this test is the wiring. The other half is the boundary: a menu is a
# proposal space, so an entry must NOT grant execution and an omission must NOT
# deny it. If that ever flips, editing a data row becomes a privilege change.
set -eu

SPG_BIN=${SPG_BIN:-build/host-debug/bin/geistshell}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

die() { echo "FAIL: $*" >&2; exit 1; }

mkrun() { # mkrun <name>
    cat > "$T/$1.spg" <<EOF
(run
 (model "fake.gguf")
 (policy "examples/policy.spg")
 (scenario "examples/scenario.spg")
 (corpus "examples/corpus.spg")
 (journal "$T/$1.sgj")
 (seed 42)
 (budgets
  (inference_steps 8) (tokens 256) (shell_actions 2) (sim_actions 8)
  (memory_actions 8) (wall_ms 10000) (journal_bytes 1048576) (risk_bp 10000)))
EOF
}

# ---- 1. the built-in menu is rendered into the context ---------------------
mkrun builtin
"$SPG_BIN" agent --config "$T/builtin.spg" \
    --fake-script examples/eval/sim_finish.txt > /dev/null 2>&1 ||
    die "a plain run must work"
"$SPG_BIN" replay "$T/builtin.sgj" --payloads > "$T/builtin.txt" 2>&1
grep -q '(tools' "$T/builtin.txt" || die "the menu must be in the context"
grep -q 'list directory contents' "$T/builtin.txt" ||
    die "the summary is what makes the menu useful to a model"

# ---- 2. the menu sits in the CONSTANT prefix ------------------------------
# It must be pinnable with the contract (#58), i.e. before the per-tick parts.
python3 - "$T/builtin.txt" <<'PY' || die "the menu must precede the per-tick context"
import sys
t = open(sys.argv[1]).read()
sys.exit(0 if t.index("(tools") < t.index("(budgets") else 1)
PY

# ---- 3. a menu FILE replaces the built-in table, no rebuild ---------------
cat > "$T/menu.spg" <<'EOF'
(command_menu
 ((name "kubectl") (summary "talk to a kubernetes cluster") (flags "get describe"))
 ((name "jq") (summary "filter json")))
EOF
mkrun file
"$SPG_BIN" agent --config "$T/file.spg" \
    --fake-script examples/eval/sim_finish.txt --command-menu "$T/menu.spg" \
    > /dev/null 2>&1 || die "a menu file must load"
"$SPG_BIN" replay "$T/file.sgj" --payloads > "$T/file.txt" 2>&1
grep -q '(kubectl ' "$T/file.txt" ||
    die "a command absent from the built-in table must still reach the model"
grep -q 'talk to a kubernetes cluster' "$T/file.txt" || die "its summary too"
grep -q 'list directory contents' "$T/file.txt" &&
    die "a menu file replaces the built-in table, not extends it"

# ---- 4. THE BOUNDARY: a menu entry does not grant execution ---------------
# `sh` is on no menu here. If it runs anyway, the menu is not an allowlist —
# which is exactly the documented, intended behaviour. This assertion exists so
# that a future change making the menu authoritative FAILS here loudly rather
# than quietly turning a data file into a permission.
cat > "$T/offmenu.txt" <<'EOS'
(recommend (kind local_shell) (capability "build.run") (cost 1) (uses_network false) (confidence_bp 6000) (reason "off-menu") (command "echo OFF-MENU-RAN"))
(recommend (kind finish) (reason "done"))
EOS
mkrun offmenu
"$SPG_BIN" agent --config "$T/offmenu.spg" --fake-script "$T/offmenu.txt" \
    --allow-exec --command-menu "$T/menu.spg" > "$T/offmenu.out" 2>&1 ||
    die "the run should complete"
grep -q 'OFF-MENU-RAN' "$T/offmenu.out" || {
    echo "FAIL: a command absent from the menu did not run." >&2
    echo "  The menu is a PROPOSAL SPACE, not an allowlist (see cmd_menu.h)." >&2
    echo "  If this was made authoritative on purpose, that is a security" >&2
    echo "  model change: an untrusted data file now decides execution." >&2
    cat "$T/offmenu.out" >&2
    exit 1
}

# ---- 5. ... and a menu entry does not survive the policy gate -------------
# `ls` IS on the built-in menu, but under a capability the policy does not
# grant. Being on the menu must not help.
cat > "$T/denied.txt" <<'EOS'
(recommend (kind local_shell) (capability "not.granted") (cost 1) (uses_network false) (confidence_bp 6000) (reason "on menu, ungranted") (command "ls"))
EOS
mkrun denied
"$SPG_BIN" agent --config "$T/denied.spg" --fake-script "$T/denied.txt" \
    --allow-exec > "$T/denied.out" 2>&1 || true
grep -q 'termination=denied' "$T/denied.out" ||
    die "the policy gate must still refuse an ungranted capability: $(cat "$T/denied.out")"

# ---- 6. the mask is opt-in and reaches the decoder ------------------------
# On the agent path the mask is a flag (--command-mask); in eval it comes from
# the model profile's (command_mask true). Both are off by default, so the
# free-decoding arm stays the control a baseline compares against.
mkrun masked
"$SPG_BIN" agent --config "$T/masked.spg" \
    --fake-script examples/eval/sim_finish.txt --command-mask \
    > /dev/null 2>&1 || die "--command-mask must run"

printf '(model_profile (name "m") (command_mask true))\n' > "$T/prof.spg"
"$SPG_BIN" eval examples/eval/suite.spg --model-profile "$T/prof.spg" \
    > "$T/prof.out" 2>&1 || die "a profile with command_mask must load: $(cat "$T/prof.out")"

printf '(model_profile (name "m") (command_mask maybe))\n' > "$T/badmask.spg"
if "$SPG_BIN" eval examples/eval/suite.spg --model-profile "$T/badmask.spg" \
        > "$T/badmask.out" 2>&1; then
    die "a non-boolean command_mask must be rejected"
fi

# ---- 7. a broken menu file fails loudly -----------------------------------
printf '(command_menu ((name "ls")))\n' > "$T/bad.spg"
if "$SPG_BIN" agent --config "$T/builtin.spg" \
        --fake-script examples/eval/sim_finish.txt --command-menu "$T/bad.spg" \
        > "$T/bad.out" 2>&1; then
    die "an entry without a summary is useless to the model and must fail"
fi
grep -q 'invalid command menu' "$T/bad.out" || die "say what is wrong"

if "$SPG_BIN" agent --config "$T/builtin.spg" \
        --fake-script examples/eval/sim_finish.txt --command-menu "$T/nope.spg" \
        > "$T/missing.out" 2>&1; then
    die "a missing menu must not run"
fi
grep -q 'cannot read command menu' "$T/missing.out" || die "say the file is missing"

# ---- 8. the name says menu, not registry ----------------------------------
# The old name read as access control, which is how it was misread for months.
grep -rq 'spg_cmd_registry' src include && die "the registry name must be gone"
grep -q 'NOT AN ALLOWLIST' include/geistshell/cmd_menu.h ||
    die "the header must state what this is not"

echo "test_cli_command_menu: PASS"
