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

## Not to be confused with geistagent

The sibling [geistagent](https://github.com/geisten/geistagent) runs the same
eval-gated discipline as an *offline* skill miner over a *closed* tool-calling
toolset, where the ground truth comes from a hand-written corpus. geistshell
learns *online* from *open* action execution, where the ground truth comes from
the world. Same gate, different source of truth.
