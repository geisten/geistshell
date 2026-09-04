# Learning from real use — the verifier

geistshell already has an eval-gated self-improvement loop (`improve`): it
distills a lesson from a failing eval case, persists it to the mind-palace,
re-runs the suite, and keeps the lesson only if nothing regressed. What it
does **not** do yet is learn from *real* task runs — because a real run has no
"expected answer" to gate against. This document is the design that closes
that gap, decided in full before implementation.

The whole point is one property: **the world supplies the ground truth, no
model judges itself.** A model that grades its own trajectory is Hermes'
documented failure mode, and on a small model with a short sequence length it
is also impossible — you cannot feed a 2000-token trajectory back for a
verdict. geistshell executes real commands, so the ground truth is free: an
exit code, an output substring, a post-check. That is why the executor runtime
is the honest place to build "learns from use."

## The pipeline, end to end

1. **Declare the criterion.** `spg_run_config` gains an *optional* `expect`
   (the existing `spg_eval_expect` shape: expected exit code + required output
   substring). Absent → the run is not a success-side learning case, but its
   terminal failures still teach through today's path. The verifier is
   `spg_eval_judge`, which already checks that shape against a finished run —
   reused verbatim, model-free, zero tokens.

2. **Reconstruct a frozen case.** A run's journaled `MODEL_OUTPUT` events are
   the fake-model script (`spg_fake_response[]`); `ACTION`/`RESULT` give the
   observation; the declared `expect` gives the verdict. Deterministically
   replayable.

3. **Reflect.** Two classes of learnable failure:
   - **Terminal** (rejected / denied / error / budget): the cause is known, so
     the lesson is actionable — today's path, unchanged.
   - **Finished-but-criterion-failed** (new — the reason the verifier exists):
     slug `(capability-set, criterion-miss)`, body quotes the *concrete*
     observed-vs-expected (`"file not found" instead of "SUCCESS"`). No invented
     fix — the fact, and recurrence judges whether stating it helps.

4. **Mint and gate.** The lesson is persisted tentatively, then re-checked
   against the shipped hand-written suite (frozen, `gate_marker`-reactive)
   **and a bounded ring of real positive guards** (finished + criterion-passed
   runs), deduped by capability-set task shape, LRU-evicted. Kept iff nothing
   regressed. **Weg 2 (chosen):** each guard is re-run **live** — a frozen
   replay of a real guard ignores context and so cannot react to the lesson at
   all, so the guard is re-run from its config with, then without, the lesson
   in the mind-palace; a guard that passed at baseline and fails with the
   lesson vetoes it. This costs live inference and loads the sequence length —
   the price accepted for catching a regression on a diverse task at mint time
   rather than only later (see decision 3).

5. **Measure benefit longitudinally.** No synthetic proof at mint time. If a
   lesson works, its failure slug stops recurring in later journals; if the
   slug keeps recurring, the lesson is flagged for revision or removal. A
   counter reads the journal; the model only ever runs in normal operation.

   `audit <journal>… --memory-dir <d>` does that counting. One journal file is
   one run, so the file count is the run count and the file's mtime is when the
   run finished; the lesson file's mtime is when it was minted. The journals
   split at that mtime and the audit reports hits-per-run on each side:

   ```
   {"journals":5,"lesson-rejected":8,"lesson-denied":0}
   {"lesson":"lesson-rejected","before":{"runs":3,"hits":6},
    "after":{"runs":2,"hits":2},"verdict":"improving"}
   ```

   The split is the whole point. A lesson exists *because* of a failure, so its
   pre-mint runs guarantee a non-zero count; a single summed total can only ever
   say "still recurring", and can never tell a lesson that works from one that
   does not. Verdicts: `pending` (no run since the mint yet — the honest state
   right after minting), `kept` (zero hits since), `improving` (a lower rate
   since), `review` (no better).

   Only the two journalled failure modes are counted, `lesson-rejected` and
   `lesson-denied`. Budget, max-steps and error terminations mint lessons but
   leave no marker in the journal, so they cannot be audited until the loop
   writes one.

## Sequence-length discipline (small models)

- **Slug-triggered auto-injection**, not model-directed recall: at the moment
  of failure — which the loop already knows — the *one* matching lesson is
  injected. No `memory_read` action a small model may never emit.
- **Description only** on small models (a one-line directive, ~40–80 tokens);
  the full `body` stays in the mind-palace for model-directed recall on larger
  models. `spg_lesson` already carries both.
- **At most one active injected lesson per tick** — the context cost is capped
  at a single directive line at any moment, however large the mind-palace grows.
- **Per-model budget knob** (like `route_min_margin`); an over-budget
  description is rejected at mint, which forces `reflect` to write terse
  directives — which it already does.

## Decisions (with the rejected alternatives)

| # | Decision | Rejected, and why |
|---|---|---|
| 1 | Verifier signal is programmatic (exit code, output substring) | Model-judged trajectory — blows a small model's sequence length; is Hermes' self-grading defect. |
| 2 | Criterion reuses `spg_eval_expect`, optional on the live run | A new type — the eval loop already consumes this one. |
| 3 | **Weg 2:** guards are re-run **live** at mint time (with vs without the lesson); a guard that passed and now fails vetoes it. Benefit is still longitudinal slug-recurrence. | Frozen replay of guards — a frozen tape ignores context, so it can never react to a lesson and thus gates nothing (found during P5 implementation). The live re-run's sequence-length cost is accepted for early regression detection on diverse tasks. |
| 4 | Slug-triggered auto-injection of the single relevant lesson | Model-directed recall — a small model may never emit `memory_read`, so lessons sit unused. |
| 5 | Frozen suite accumulates real positive guards | Shipped hand-written suite only — cannot represent diverse ad-hoc script tasks; the longitudinal signal is too slow to catch a broken rare type. |
| 6 | Task-shape key = capability set `(action_kind, capability-class)` | Scenario name — ad-hoc runs have none. `expect` hash — collides unrelated tasks that share "exit 0". |
| 7 | Finished-but-wrong runs mint a lesson quoting the concrete miss | Learning nothing from them — wastes the class the verifier was built for. Model-reflected correction — sequence length + self-grading. |
| 8 | Inject description only on small models, one slot per tick | Full body always — overflows a short window. Truncated body — the description is the deliberate compression; truncation invents a worse one. |

## Deliberately open — practice decides

- **Sequence over set (decision 6): mechanism SHIPPED (#12), default
  unchanged.** `spg_shape_from_script_mode(..., SPG_SHAPE_MODE_SEQUENCE, ...)`
  builds the finer key — the ordered `'>'`-joined sequence of
  `<kind>:<capability>` tokens, consecutive duplicates collapsed, `finish`
  excluded, deterministically truncated at `SPG_SHAPE_MAX_TOKENS`. The SET key
  stays the default everywhere (cheaper, more bounded); switching a caller to
  the sequence key remains trigger-gated on an observed guard collision — two
  distinct tasks sharing a guard where a lesson broke one but the guard was
  the other.
- **Post-check command (decision 2):** a last-step shell probe (`exit 0 =
  success`) verifying world state, for tasks that produce files rather than
  stdout.

## Constrained decoding, forced-prefix first cut (geistshell#34, 2026-08-02)

The exemplar finding pointed to grammar-constrained decoding as the real
unblock. First cut: force the GEIST model's output to BEGIN with a fixed
literal (`(recommend (kind `), then decode freely — using the engine hooks
the #34 pre-clarification found (`geist_session_tokenize` +
`geist_session_prefill_tokens`, v0.2.1 `geist_util.h`, no engine fork).

Measured on the Pi against Gemma 4 E2B, the reject reason MOVED:

- free decode / exemplars only: `REJECT_SYNTAX` — not even a parseable form.
- **forced prefix (`--constrained`): `REJECT_UNKNOWN_KIND`** — the output now
  parses as a recommendation up to the kind field; Gemma only picks an invalid
  kind name after the forced `(recommend (kind `.

That is real, measured forward progress: forcing the opening carries the model
past the structure it could not start, and pinpoints the exact next
constraint.

**Stage 1 — kind-enum mask (merged).** After the forced prefix ends at
`(kind `, a greedy `peek_logits` loop masks the next tokens to only those that
keep the emitted text a live prefix of a valid kind name
(`spg_action_kind_to_string` is the source of truth, so the mask cannot drift
from `parse_action_kind`), until a complete name is reached. Pure predicates in
`grammar_mask.c`, unit-tested without a GGUF. On the Pi the reject moved again:

- forced prefix only → `REJECT_UNKNOWN_KIND` (Gemma picks an invalid kind).
- **+ kind mask → `REJECT_MISSING_FIELD`** — output `(recommend (kind
  simulator))`: a *valid kind by construction*, now missing the required
  fields. `termination=finished`, not `budget` — the model lands a parseable
  form immediately.

Subtlety found on hardware: `token_to_str` detokenizes the SentencePiece
word-boundary to a leading ASCII space/tab, so the first kind token reads
`" simulator"`; the mask compares past leading whitespace (the separator stays
in the output, valid in the s-expression). Without that, every candidate was
rejected and the loop emitted `(recommend (kind ))`.

**Stage 2 — field scaffold (merged).** Once the kind resolves, the rest of a
valid form is deterministic structure with a few free leaf values, so the
adapter emits that structure itself (`emit_literal` → `prefill_tokens`) and lets
the model free-decode only the string slots (`decode_string_slot`, stopped at
the closing quote). The bureaucratic fields (`cost`, `uses_network`,
`confidence_bp`) are baked into the scaffold literals with defaults —
deterministic per kind, not a decision a 2B model should have to land. The
per-kind field sets mirror `required_fields_seen` + `kind_fields_match`, so a
scaffold is valid by construction; `test_grammar_mask` builds each of the seven
kinds' forms and parses them through the real `recommendation.c` — all VALID,
no GGUF.

On the Pi the whole reject chain is now resolved:

`SYNTAX` (free) → `UNKNOWN_KIND` (prefix) → `MISSING_FIELD` (kind mask) →
**a valid form** (scaffold). Gemma emitted, in **one step / ~22 s** (vs ~2m20
of repair loops before):

    (recommend (kind simulator) (capability "read") (cost 1)
      (uses_network false) (confidence_bp 5000)
      (reason "Simulate a general system behavior"))

Parses and matches the schema — the model now emits the DSL. `termination`
is `denied`, not a decode failure: the form reached the **policy gate**, which
denied capability `"read"` (the policy allows `sim.act` / `build.run`). That is
acceptance criterion #1 met — a valid, schema-matching form where free decoding
gave SYNTAX.

**Stage 2b — capability mask (merged).** The kind mask generalized to a choice
mask over any candidate list (`spg_choice_*`); the capability slot is now masked
to the policy's enabled capabilities for the chosen kind (the adapter is handed
the enabled caps, memory expanded to each `memory_*` kind). Inside a quoted
string whitespace is literal, so `decode_choice_slot` strips the leading detok
space — the value is emitted exactly (`"sim.act"`, not `" sim.act"`, which the
policy would not match).

On the Pi the deny cleared and the action executed end to end:

    (recommend (kind simulator) (capability "sim.act") (cost 1)
      (uses_network false) (confidence_bp 5000) (reason "Simulate a scenario"))
    (policy_decision (decision allow) (deny_reason SPG_POLICY_DENY_NONE) ...)
    (sim_result (action patch_vulnerability) (mutated true)
      (risk_before 35300) (risk_after 32100))

The policy **allowed** the action and the simulator **ran**, dropping risk — a
real effect from a model that could not emit the DSL at all before constrained
decoding. The full chain is resolved: `SYNTAX → UNKNOWN_KIND → MISSING_FIELD →
valid form → denied → allowed + executed`.

That is the decoder complete: a small model reliably emits a valid,
policy-grantable, executable recommendation under `--constrained`. `termination`
was `max_steps` (Gemma kept choosing simulator actions rather than `finish`) —
a behaviour/corpus matter, not a decode gap. What remains is the **LIFT
measurement** (#25 numerator, #26 benefit): a corpus with an `expect` criterion
where control (free decode, scores ~0) vs constrained can be compared for
success rate and gain-per-token. That is the payoff the whole decoder was built
to unblock, and it is now unblocked.

## LIFT measured (stage 3, geistshell#34/#25, Pi against Gemma 4 E2B)

geistshell has no free-text goal — the task IS the security scenario, and the
model recommends risk-reducing actions. `eval/bench_lift.sh` runs four scenario
variants (rising vulnerability severity) twice each: free decode vs
`--constrained`, real model, ~18 min on the Pi.

| task | free valid | constr valid | free acted | constr acted | free done | constr done |
|---|---|---|---|---|---|---|
| sev=4000 | 0 | 1 | 0 | 1 | 0 | 0 |
| sev=6000 | 0 | 1 | 0 | 1 | 0 | 0 |
| sev=8000 | 0 | 1 | 0 | 1 | 0 | 0 |
| sev=9500 | 0 | 1 | 0 | 1 | 0 | 0 |
| **total** | **0/4** | **4/4** | **0/4** | **4/4** | **0/4** | **0/4** |

- **valid** — a valid, policy-allowed recommendation (`(policy_decision (decision
  allow ...)` in the journal).
- **acted** — an action actually executed and moved the world (a `(sim_result
  ...)` in the journal): the honest task-success signal.
- **done** — `verdict=pass`.

The LIFT is unambiguous: free decode **0/4**, constrained **4/4** on both valid
emission and real execution. Constrained decoding is what turns a 2B model that
cannot emit the DSL at all into one that reliably proposes valid, grantable,
executing actions — the #25 numerator, measured.

`done` (verdict=pass) is **0/4 in both arms**, and honestly so: the CLI's expect
judge requires `termination=FINISHED` (main.c), i.e. the model must emit `(kind
finish)`. Greedy Gemma keeps choosing simulator until the step cap, so it acts
but never declares completion. Emitting a valid action and choosing to STOP are
separate behaviours; the decoder fixed the first, not the second. Closing that
gap (e.g. a finish nudge once risk is under threshold, or a terminating corpus)
is follow-on behaviour work, not a decoder gap — the constrained decoder's job,
lifting DSL emission and execution from 0 to 100%, is done and measured.

## Convergence stop closes acted -> done (geistshell#40)

The termination gap above, fixed at the loop. A model that emits valid,
executing actions but never `(kind finish)` used to run to the step cap
(`termination=budget`), so `verdict=pass` — which needs `FINISHED` — stayed 0
even when the action executed. `spg_agent_loop_config.finish_on_no_progress`
(opt-in; the CLI enables it) treats a step that makes no progress as
completion: a simulator action the executor turned into a **noop**
(`sim.mutated == false` — nothing left to change) terminates the run FINISHED;
other actions use a repeated non-empty observation.

Two fixes were needed together, both found on hardware:

- **the no-progress signal.** A first cut compared the observation buffer, but
  sim actions never write it, so the stall never fired on the sim corpus. The
  honest signal is `sim.mutated == false`.
- **the observation channel.** Sim wrote only `sim_payload` + the journal, never
  the shared observation buffer, so the model acted *blind to its own outcome*
  and an expect verdict had nothing to match (`fail_observation` even once
  FINISHED). Mirroring the sim result onto `observation_buf` (as shell/memory
  already do) both feeds the model its result and lets the verdict see it.

Re-running `bench_lift.sh` with the fix, `done` converts:

| | free valid | constr valid | free acted | constr acted | free done | constr done |
|---|---|---|---|---|---|---|
| **total** | 0/4 | 4/4 | 0/4 | 4/4 | **0/4** | **3/4** |

`done` went **0/4 → 3/4**. The one miss is `sev=9500`, the highest-severity
scenario: it takes more actions to exhaust than the step budget allowed, so the
sim had not yet returned a noop when the budget ran out — an honest convergence
depth, not a broken mechanism (a larger budget would finish it). Acting validly
and choosing to stop are now both handled: the decoder emits valid actions
(#34), and the loop terminates cleanly when the agent has nothing left to do
(#40).

## Does an injected lesson lift a model that can now act? (2026-08, Pi/Gemma)

With #34 + #40 the model finally *acts*, so the learning claim can be probed
directly: seed a lesson into the mind-palace (it renders into every prompt via
`render_memory_index`) and compare against an empty store, constrained decode,
greedy (seed 42, deterministic).

**Injected lessons causally steer the acting model — measured.** On the sev=8000
scenario, a blunt "emit finish immediately" lesson flipped the run from
`steps=4 finished pass` (control) to `steps=3 denied fail` (learned). The
directive reached the model through the index and changed its choice. The
substrate works: a stored lesson is not inert on a model that acts.

**But the effect is unreliable and can regress — which is exactly why the
eval-gate exists.** That same lesson made success *worse* (pass → denied), and
on sev=9500 a targeted "finish after patching" lesson had no effect at all
(denied → denied). A naive lesson sometimes hurts, sometimes does nothing. The
keep-only-if-no-regression gate (decision 3) is not ceremony — a live lesson
here would have to be vetoed.

**The security-scenario corpus lacks clean success headroom.** Outcomes are
mostly decided by the *sim executor's* action selection, not by the model's own
decisions, so there is little the model freely chooses that a lesson can shape
toward a reliable positive lift. Free decode still scores ~0 (can't emit the
DSL), and constrained decode already succeeds where the objective is reachable —
so the arm that would show lift (control fails, lesson fixes) barely exists on
this corpus.

Honest conclusion: the learning substrate is **live** (lessons reach and steer
the acting model) and the gate is **necessary** (lessons can regress), but a
convincing *positive* learning-LIFT needs a corpus where the **model's own
decision determines success** — the local_shell command-shape (`bench_skills`),
where the lesson supplies the command the model must produce, run against the
real model. That is the next experiment; the security-simulator corpus was the
wrong instrument for it. The runtime auto-injection path (`lesson-rejected` on
repair) is separately bypassed by constrained decoding, so a semantic
lesson's only channel to an acting model is the index — which this probe shows
does carry, and steer.

### The goal-enabled retest — real headroom, lesson still doesn't lift

The `(goal "...")` field (added next) gave the missing headroom: a goal-directed
shell task where the *model's own decision* determines success. Goal "Run a
local shell command that prints READY" steers Gemma cleanly — it emits `(kind
local_shell) (capability "build.run") (command "echo READY")`, allowed, and
READY is printed. But then it **repeats** the command and the second `build.run`
is budget-denied (`build.run` has budget 1 in the policy), so the run ends
`denied`, never `finished` — a real, reproducible failure with genuine headroom:
the model must emit `(kind finish)` after achieving the goal.

Control vs learned (a "finish once the goal output is produced" lesson in the
mind-palace), constrained, greedy:

| | termination | verdict |
|---|---|---|
| control | denied | fail |
| learned | denied | fail |

**No lift.** The index-resident directive did not make greedy Gemma emit
finish. Combined with the earlier probe (where a lesson *did* bite — negatively),
the honest conclusion is: on a 2B greedy model the one-line index directive is
**too weak a channel to reliably steer high-level behaviour** (like choosing to
stop), and when it does bite it is as likely to regress as to help — which is
exactly why the keep-only-if-no-regression gate exists.

### Standing conclusion for small models

Across every probe, the pattern holds: **deterministic guardrails carry the
weight, learned directives do not** on this model. Constrained decoding (#34),
choice-slot force-completion, the convergence stop (#40), and goals each removed
a failure mode *reliably*; the same failure modes are what a lesson would target,
so the guardrails largely **subsume** the learning headroom. The learning
substrate is real and the gate is necessary, but a convincing positive
learning-LIFT on a small greedy model would need either a **stronger injection
channel** (the directive prepended to the observation every step, not buried in
the index — untried) or a **larger model** that attends to directives. On small
models, invest in guardrails; keep the eval-gate as the safety net for when a
lesson does bite. (The goal-directed shell task above would itself pass with a
deterministic "budget-denied repeat after a success = converged → finish" rule —
another guardrail, not a lesson.)

### The two follow-ups, tested (Pi/Gemma)

Both levers the conclusion named were then built and measured on the
goal-directed shell task (goal "print READY", `build.run` budget 1):

- **Stronger channel (Weg 1) — failed.** A designated lesson's directive is now
  rendered prominently as `(directive "...")` at the top of the context *every
  step* (not the mind-palace index). Confirmed in the prompt — and greedy Gemma
  still does not emit `(kind finish)`: control and learned both end `denied`,
  both `fail`. A strong, explicit, always-present directive changes nothing. On
  a 2B greedy model, directive-based steering of high-level behaviour does not
  work regardless of channel strength. (The channel ships anyway — it is the
  right mechanism for a model that *does* follow directives.)
- **Deterministic guardrail (Weg 2) — works.** When a step is budget-denied
  (capability or global) *after* the agent already executed an allowed action,
  it has spent its allowance and is repeating completed work — converged, not
  failed → FINISHED. The goal task now passes with **no lesson**:
  `termination=finished, verdict=pass` (control). A budget denial with no prior
  progress stays a real DENIED.

### In-conversation example carry (Hebel 1) — hurts, and here is why

The directive failure suggested a different form of learning that ought to suit
a small model better: not an abstract lesson but **the model's own verified
successes carried as few-shot examples** into later turns of the same session
(imitation of concrete recent examples, the half this project measured as
*working*). `bench_carry.sh` runs a sequence of same-shape goals (print a word
via local_shell); each turn the verifier passes contributes its emitted
`(recommend ...)` form to the `(examples ...)` slot the next turns see. Reuses
the existing exemplar channel — no engine change.

Measured on the quiesced Pi:

    no-carry: 5/6   READY:P DONE:P OKAY:.  FINE:P GOOD:P SET:P
    carry:    3/6   READY:P DONE:P OKAY:P  FINE:.  GOOD:. SET:.

**Carry made it worse (5/6 → 3/6), and progressively — every later turn fell
once the exemplars accumulated.** The mechanism is confirmed directly: with the
READY/DONE/OKAY forms carried, goal *FINE* emitted `(command "echo READY")` — it
**copied a prior example's content** instead of following the current goal. A
small model imitating concrete examples cannot separate "copy the *structure*"
from "copy the *content*"; the verified example poisons the next task with the
previous task's answer. (Individual borderline tasks are thread-noisy, so the
exact count is soft, but the direction and the copy mechanism are clear.)

So even the *right-shaped* learning — concrete, recent, verified — backfires on
a 2B model. Directives are ignored; examples are over-imitated. Both forms of
runtime in-context learning fail; the guardrails and best-of-N do not.

This is the conclusion made concrete: the learned path was given its strongest
possible form and still did not lift; the deterministic rule did. On small
greedy models, build guardrails.

## Verifier-guided best-of-N (measured, Pi/Gemma)

The one deterministic lever left: geistshell has a verifier (policy gate +
`expect` + the world) but ran a single greedy trajectory. Best-of-N is the
sampling counterpart of constrained decoding — stochastic attempts, deterministic
selection. For it to bite, the model's *decision* must vary, so the masked
choice slots (kind, capability) now take a softmax draw at temperature instead of
the argmax; different seeds explore different **valid** decisions and the
verifier keeps the first run that passes (`bench_bestof.sh`, T=0.8, N=4).

**First measurement was contaminated — corrected below.** The initial run
reported best-of-N 2/5 vs greedy 0/5, but two confounds invalidated it:
1. **A harness bug.** The competing-capability policy for the shell corpus used
   `(network deny)` (not `network_default`) and a wrong `budgets` order →
   `SPG_E_SCHEMA`, so every shell run instant-failed at policy load in ~4 ms.
   The "shell 0/N" was a broken policy, not a model result.
2. **A non-quiesced box.** The Pi ran the always-on `geist --serve` home daemon
   the whole time (the [quiesce-the-box] lesson, ignored). Greedy's outcome on
   the borderline tasks turned out to depend on the OMP thread count — with the
   daemon holding a core, geistshell got a different thread count, a different
   fp reduction order, a different argmax, a different trajectory. Greedy flipped
   pass↔fail across sessions on identical config.

Fixed both: `examples/policy.spg` already enables both kinds (it *is* the
competing-capability policy), and the home daemon was stopped and disabled.
Re-run three times on the quiesced box (`bench_bestof.sh`, N=6, T=0.9):

| | greedy | best-of-6 |
|---|---|---|
| simA sev=4000 | fail | pass (attempt 2) |
| simA sev=8000 | fail | pass (attempt 1) |
| shellB READY | pass | pass |
| shellB DONE | pass | pass |
| shellB OKAY | fail | pass (attempt 2) |
| **total** | **2/5** | **5/5** |

**Reproducible — three runs byte-identical.** Best-of-N recovers all three greedy
failures within 1–2 attempts. This is the clean, positive result; the earlier
number was noise.

Two properties worth stating. (a) The engine's greedy decode is **not
deterministic across thread counts** — argmax depends on the fp reduction order,
which depends on how many OMP threads run. It is deterministic within a fixed
quiesced box (hence the 3× identical runs), which is now the canonical
measurement state. (b) The `expect` criterion is trajectory-*endpoint* sensitive
(checks the last observation), so the two simulator failures are partly greedy
ending on the wrong action, not doing the wrong thing — a corpus weakness, not a
best-of-N one. The shell `OKAY` failure is a genuine task miss that best-of-N
recovers.

The direction is clear and reproducible: let a fast weak model attempt several
times and let the verifier — not the model — decide. Best-of-N is the sampling
form of the same principle as constrained decoding; unlike learned directives,
it moved the number, and this time the number holds up.

Dev-env note: the Mac's deps/geist pointed at a newer checkout lacking
geist_util.h; switched to the pinned v0.2.1 via `make sync-engine`.

## Extending constrained decoding (#1): parens in string values; full acceptor deferred

The per-kind scaffold already takes valid emission from 0 to 100% on the
recommendation DSL, so a general **token-level grammar acceptor** — masking every
token against a live parser state — is deferred as YAGNI: its only benefit is
generality for a *second* grammar (the natural place is the geistagent
tool-calling path, if/when the two runtimes unify), and it is a large,
subword-boundary-hard rewrite for no gain on the DSL we have.

The concrete extension that *does* add capability: `decode_string_slot` used to
stop a string value at `"`, newline, **or a paren**. But inside an s-expr string
parens are literal (sexpr.c only forbids raw `"`, newline, and stray
backslash-escapes), so stopping at parens needlessly banned real commands —
`awk '{print}'`, `grep (x)`. Relaxed the stop set to `"`/newline only; the whole
string-slot family (command, target, reason, slug, description, body) now admits
parens. `test_valid_command_with_parens` confirms such a form parses VALID, so
the relaxation cannot emit an unparseable value. Raw `"` inside a value still
ends the slot (the model would need `\"`); auto-escaping is a further step.

## Real-model completion: exemplars are necessary but not sufficient (2026-08-02)

Path B added an `(examples ...)` context slot so a small model gets concrete
worked recommendation forms, not just the `(contract ...)` grammar. Measured
on the Pi against Gemma 4 E2B, two runs on the same goal:

- **without exemplars:** terminates `budget`, never a valid form (as before).
- **with exemplars:** still terminates `budget`; the repair path reports
  `SPG_RECOMMENDATION_REJECT_SYNTAX` — Gemma emits something, but it does not
  parse as a valid s-expression form.

Honest conclusion: **few-shot exemplars alone do not make a 2B model
DSL-completable here.** The exemplar mechanism is correct and merged (it
renders in context, unit-tested), but a 2B model cannot freely emit a novel
structured DSL from examples within a few steps. The proven fix for exactly
this class — a small model emitting a structured call — is
**grammar-constrained decoding** (geistagent forced tool-call output along the
grammar and got BitNet/Gemma to emit valid calls). geistshell has no such
sampler mask. So the real-model LIFT measurement (#25 numerator, #26 benefit)
is gated on grammar-constrained decoding, an engine-level piece — not on more
exemplars. Filed separately.

## Success-side skill abstraction (geistshell#26, first cut)

The success-side counterpart to reflect: a PASSING trajectory is distilled into
a reusable `skill-<shape>` procedure, keyed by the P4 capability shape, and
injected into later same-shape tasks via the mind-palace index the agent
already renders. Deterministic (a template over the ordered action kinds), and
OFFLINE — the live agent never self-mints skills (the anti-delusion stance);
`geistshell distill <passing-journal>` is the explicit mint step.

`eval/bench_skills.sh` shows the before/after over ten shell-script tasks that
share one shape:

- **before:** skill injected into 0/10 tasks.
- **after:** a skill distilled from the first pass is injected into 9/10.

Task-success is identical across before/after because a *fake* model ignores
the injected skill — whether the skill LIFTS success needs a real model
reacting to it (the #25 numerator, on a model-completable corpus). This proves
accumulation + injection, not lift. Open follow-ups on #26: model-driven
distillation, shape-triggered single-skill injection (vs the whole index), and
a gate on skill acceptance.

## User-profile memory (geistshell#28)

Task lessons and skills are about the WORLD (a failure the world proved, a
procedure that worked). A **preference** is about the USER — how they want
choices made — and is a distinct memory KIND, stored under a `pref-<key>` slug
namespace alongside `lesson-*`/`skill-*` but never overloading them.

Two boundaries are enforced in code, not just documented (`src/memory/pref.c`,
`test/test_pref.c`):

- **Write-on-evidence, never model self-assertion.** `spg_pref_should_write`
  is the single gate: a repeated choice writes only at the second observation,
  a user correction is authoritative on the first, and a model self-assertion
  (`SPG_PREF_EVIDENCE_ASSERTED`) writes *nothing*, whatever the count. The same
  anti-delusion stance as eval-gated learning — a preference is recorded when
  the world shows it, not when the model guesses it. The CLI surface is
  `geistshell memory pref <key> <value> --evidence … --count …`.

- **Capability-invariance.** A preference shapes FRAMING and DEFAULTS only — it
  is rendered as one `(profile "…")` context line and there is no code path
  from a preference to the policy gate. `test_cli_pref.sh` proves it: a
  `pref-allow_shell` naming shell access is injected into a plant run's
  context, yet the plant policy (device capability only) still denies a
  `local_shell` action. Personalization changes how a choice is elicited,
  never what is permitted.

The profile is one budgeted line however many preferences accumulate
(`spg_pref_render`, capped at `SPG_MEM_DESC_MAX`) — the same
context-invariance property as the P6 lesson directive, so a filling profile
never grows the small model's window. Off-switch: `agent --no-profile` (or an
empty store) leaves the context byte-identical. Orthogonal to #26/#27:
personalization, not skill.

## Context-cost benchmark (2026-08-02, model-free)

`eval/bench_context.sh` measures the load-bearing half of the small-seq-len
claim: the context cost of learning per tick as the lesson set grows.

| lessons | geistshell (directive) | full-index RAG |
|---|---|---|
| 1  | 72 B | 90 B |
| 4  | 72 B | 360 B |
| 8  | 72 B | 720 B |
| 16 | 72 B | 1454 B |
| 32 | 72 B | 2233 B |

geistshell's per-tick learning cost is **flat** (one budgeted directive)
while the mind-palace-index approach grows linearly — 31× more context at 32
lessons, and widening. This substantiates the *denominator* of the SOTA
claim (learning is context-invariant). It does **not** measure success lift:
the numerator (does flat context still improve task success?) needs a
model-completable task corpus and real inference — the remaining half of #25.
A benchmark that measures only the free half must say so.

**The numerator arm (#25):** `eval/bench_learning_gain.sh` is the reproducible
three-arm harness for the success side — control (no memory) vs geistshell
(one directive/tick) vs full-index RAG (whole index/tick) — as the lesson set
accumulates across passes, reporting task-success rate and context-tokens/tick
per arm so the headline gain-per-context-token can be computed. It is
model-gated (a real GGUF, not `make test`) because a fake model ignores
injected learning, and it is written to be able to **refute** the claim: a
null lift retires this paragraph rather than keeping it unproven. The number
itself is produced on a model host; the harness and its honest framing are the
deliverable here.

The benchmark also surfaced and fixed a real bug: `spg_mem_directive`
originally read the *capped* index, so a slug beyond `SPG_MEM_INDEX_TOPK`
injected nothing; it now reads the description from the memory file, available
for any slug.

## Hardware verification (2026-08-02, Pi 5 against Gemma 4 E2B)

The model-dependent paths, run live through the whole stack (release build,
real GGUF, ~1 min/turn):

- **Real-model journaling run** — `agent --config <run>` with no `--fake-script`
  loads the GGUF and journals; Gemma, untrained for the `(recommend ...)` DSL,
  emits an invalid form and the loop repairs, exactly the P6 trigger.
- **P7 verified** — `audit` over the real journal counts the rejection:
  `{"lesson-rejected":1}`.
- **P6 verified** — with a kept `lesson-rejected` in the store, the real repair
  observation LEADS with the earned directive ("Emit exactly one valid
  recommendation s-expression …") then the generic hint; without the lesson,
  only the generic hint. Slug-triggered injection into a real context, proven
  both ways.
- **P5 veto** — the live guard runner reuses this now-proven real-model path;
  the gate logic is unit-tested. A live *veto* demonstration needs a task the
  model can actually complete (so a guard can pass and then be broken by a
  lesson); the example DSL scenarios are not model-completable by an untrained
  Gemma, so a contrived breaking task is future work.

## Directive × best-of-N — ceiling, question undecided (2026-08-16, Pi/Gemma, engine v0.9)

Every directive probe above ran **greedy**, where a directive must flip the
argmax to show at all. Under temperature sampling it only has to *shift
probability mass*, and the best-of-N verifier amplifies any shift into a
measurable difference in attempts-to-pass. `eval/bench_directive.sh` measures
exactly that: the six goal-word shell tasks, control (no lesson) vs learned
(a "finish after the goal word" lesson rendered as the Weg-1 strong channel
`(directive "...")` every step), constrained, N=6, T=0.9, three reps, quiesced
box, engine pinned at geist v0.9.0.

| | pass | attempts_sum |
|---|---|---|
| control | 18/18 | 18 (minimum) |
| lesson  | 18/18 | 18 (minimum) |

**Every run passed on the first sampled attempt in both arms — a ceiling.**
Attempts-to-pass can only differentiate when control sometimes fails; here the
metric sat at its floor for all 36 runs, so the question this bench was built
to answer — does the directive shift sampling mass? — is **undecided**, not
answered. The honest verdict is "no measurable signal on this corpus", not
"channel dead".

The ceiling itself is a finding. In the corrected best-of-N measurement
(v0.2.1, before Weg 2) greedy still failed 2/5 of these tasks and OKAY needed
attempt 2; since then the Weg-2 guardrail (budget-denied repeat after a success
= converged → FINISHED) and the v0.9 engine landed, and together they remove
this corpus's entire failure mode deterministically — the standing conclusion
(*guardrails subsume the learning headroom*) now holds **completely** on this
corpus, reproduced at T=0.9 over 18 runs.

One genuine side result: the strong-channel directive was present every step
of 18 runs and regressed **nothing** — earlier probes showed a lesson can flip
pass → denied, so "at worst harmless" is new information about the channel,
and context for how often the keep-gate must actually veto.

Consequence for the lever order (directive-under-sampling → structure-carry →
lesson-as-slot-bias): before any of them can be decided, the corpus needs
**measured headroom** — tasks where control demonstrably fails sometimes under
sampling and the failure mode is *not* guardrail-covered (multi-step goals:
print A, then B, then finish; or budgets tight enough that the first wrong
decision costs the run). Until that corpus exists, every runtime-learning
measurement here will read as a ceiling.

## Headroom census — the lever-measurable corpus exists, and it is narrow (2026-08-16, Pi/Gemma)

The ceiling above demanded a corpus where control *sometimes* fails. The
census (`eval/bench_headroom.sh`) probes transform/compute shell tasks whose
expected output is a function of the input — not producible by echoing the
goal back, not rescuable by any guardrail; only the model's command choice
decides. Per task: one greedy run + six sampled runs (T=0.9, seeds 1..6),
constrained, quiesced box. Classification: `ceiling` (6/6), `floor` (0/6),
`HEADROOM` otherwise. (The census spanned a Pi reboot mid-run; `reverse` was
measured in both sessions with the identical result.)

| task | recipe needed | greedy | sampled | verdict |
|---|---|---|---|---|
| echo a goal word (prior bench) | copy a literal | pass | 18/18 | ceiling |
| twice (`MIRROR-MIRROR`) | compose goal literals | fail | 1/6 | **HEADROOM** |
| seq (`1 2 3 4 5`) | compose counting literals | fail | 1/6 | **HEADROOM** |
| sum (17+25 → `42`) | compute, then echo | fail | 0/6 | floor |
| count (letters → `7`) | `wc` or count | fail | 0/6 | floor |
| upper (`cloud` → `CLOUD`) | `tr` | fail | 0/6 | floor |
| reverse (`STONE` → `ENOTS`) | `rev` | fail | 0/6 | floor |

**The difficulty landscape on a 2B constrained model is nearly binary.**
Echoing a literal from the goal is a ceiling; anything requiring a tool
recipe (`tr`, `rev`, `wc`) or an internal computation (17+25) is a hard
floor — across 24 sampled runs not one tool invocation succeeded. The only
band in between is **literal composition**: the answer's parts are in the
goal, but arranging them (`MIRROR-MIRROR`, `1 2 3 4 5`) succeeds in ~1/6
samples. That band is real, reproducible — and two tasks wide.

Two consequences. First, the levers are now decidable: on `seq`/`twice`,
control fails 5/6, so a directive that carries the exact recipe ("emit
`echo 1 2 3 4 5`") has genuine room to lift, and attempts-to-pass has dynamic
range — re-running `bench_directive.sh` over this corpus is the pending
Hebel-1 measurement. Second, the floor class bounds what any prompt-side
learning can achieve here: a lesson cannot teach this model to *use a tool*
it never samples; that class needs either the skill-as-forced-prefix
mechanism (decode-side, #26 follow-up) or a larger model. Widening the corpus
means minting more literal-composition variants, and each needs its own
census row before it counts as headroom.

## Directive × best-of-N on the headroom corpus — first positive lift (2026-08-17, Pi/Gemma)

`bench_directive_headroom.sh`: the two census HEADROOM tasks, each with a
lesson whose description IS the exact recipe ("Emit the shell command: echo
1 2 3 4 5"), rendered as the Weg-1 strong channel every step. Control vs
lesson, best-of-6, T=0.9, three reps, quiesced box, engine v0.9.0.

| task / rep | control pass/att | lesson pass/att |
|---|---|---|
| seq 1..3 | 1/1, 1/2, 1/1 | 1/2, 1/1, 1/1 |
| twice 1 | 1/4 | 1/2 |
| twice 2 | **0/6** | **1/1** |
| twice 3 | 1/2 | 1/2 |
| **total** | **5/6 pass, 16 attempts** | **6/6 pass, 9 attempts** |

**The directive shifts sampling mass — the first positive lift a lesson has
ever shown in this project.** On `twice`, the task with real headroom, the
recipe directive dominated every rep (4→2, 6→1, 2→2 attempts) and converted
the one run control lost outright. `seq` tied exactly (best-of-6 turned out
to be near-ceiling there; the census's single-shot 1/6 understated it) and
contributes no signal either way.

The resolution of the greedy-era conclusion: a directive **cannot flip the
greedy argmax** (every earlier probe), but under temperature sampling it
**biases the draw**, and the best-of-N verifier converts that bias into fewer
attempts and recovered runs. Lessons on a small model are a *sampling prior*,
not a command — they pay off only in the sampled+verified regime, which
best-of-N (#2) already made the production path for borderline tasks.

Honest bounds: n=6 paired runs, and the signal lives in one task — `twice` —
because the corpus's headroom band is that narrow. The direction was
consistent across all three reps and includes a 0/6→1/1 conversion, but
widening the literal-composition corpus (each new task census-verified first)
is what would turn "first positive lift" into a load-bearing number. Next
per the lever ladder: structure-carry (Hebel 3) and lesson-as-slot-bias
(Hebel 2), both now decidable on this corpus.

## Not to be confused with geistagent

The sibling [geistagent](https://github.com/geisten/geistagent) runs the same
eval-gated discipline as an *offline* skill miner over a *closed* tool-calling
toolset, where the ground truth comes from a hand-written corpus. geistshell
learns *online* from *open* action execution, where the ground truth comes from
the world. Same gate, different source of truth.
