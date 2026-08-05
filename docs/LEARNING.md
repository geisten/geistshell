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

- **Sequence over set (decision 6):** the finer shape key, if too many distinct
  scripts share one capability set and a guard misses regressions.
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

Dev-env note: the Mac's deps/geist pointed at a newer checkout lacking
geist_util.h; switched to the pinned v0.2.1 via `make sync-engine`.

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

## Not to be confused with geistagent

The sibling [geistagent](https://github.com/geisten/geistagent) runs the same
eval-gated discipline as an *offline* skill miner over a *closed* tool-calling
toolset, where the ground truth comes from a hand-written corpus. geistshell
learns *online* from *open* action execution, where the ground truth comes from
the world. Same gate, different source of truth.
