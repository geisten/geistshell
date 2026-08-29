# Step-4 Baseline — the ladder, measured with and without the harness

First run of the [MODEL-ADAPTATION.md](../MODEL-ADAPTATION.md) decision-4 suite
(7+7 cases, train + hold-out) as a controlled harness ablation: the same
suite, seed, budgets and fixtures, decoded once `--constrained` (the shipped
decoder) and once free. Measured 2026-08-29 on the Pi 5 (host-release build,
commit 824ed4b).

## Method

- **Suites:** `examples/eval/bench/model_train.spg` / `model_holdout.spg`.
- **Arms:** `--constrained` (per-kind scaffold + token mask) vs free decode.
  Only the flag differs; everything else is identical per pair.
- **Budgets (v2):** tokens 1536, inference_steps 8, wall 360 s. The first
  attempt (v1) ran the configs' stock `tokens 512` and was **token-bound**: all
  budget-terminated cases stopped at identical step counts (~3) across samples
  — the suite measured the budget, not the model. v1 is kept below as the
  diagnostic; every conclusion is drawn from v2. Raising the budget moved the
  terminations from `budget` to `max_steps` and changed no verdict — the
  failure is real, not an artefact ([#126](https://github.com/geisten/geistshell/issues/126)
  adds per-case `wall_ms`/`tokens` so this class of artefact is visible in the
  JSONL directly).
- **Sampling:** `--samples 3` (gemma) / 5–10 (smaller models), `--temperature
  0.8`, seed 42. Fixture isolation per sample (pristine copy + fresh
  mind-palace store before every run).
- **Models:** gemma4-e2b Q4_K_M (control). qwen3-0.6b / qwen3.5-0.8b
  **could not run**: the pinned engine does not load the qwen3 architecture
  (`SPG_E_MODEL`; support sits in geistlib#236). bitnet_b1_58-3B **INT_N**
  also fails to load — that is bitnet.cpp's format, not libgeist's `I2_S`;
  the I2_S/TQ2_0 variants are queued (v2b/v2c) and land in the table below
  when done.

## Ladder (v2, gemma4-e2b, N=3 per case)

| Suite | Arm | parse | gate | task | wall |
|---|---|---|---|---|---|
| train | constrained | **17/21 (81%)** | 14/21 (67%) | 3/21 (14%) | 45.5 min |
| train | free | **0/21 (0%)** | 0/21 | 3/21* | 22.0 min |
| hold-out | constrained | **14/21 (67%)** | 11/21 (52%) | 3/21 (14%) | 35.1 min |
| hold-out | free | **0/21 (0%)** | 0/21 | 3/21* | 22.0 min |

\* vacuous: only `denied-probe` passes in the free arm, and it passes because
its deliberately termination-agnostic expectation (`(max_steps 4)` only) also
accepts a step-1 parse rejection. The constrained arm passes the same case
genuinely (termination `denied` — the gate rung working). Every other free-arm
run dies at step 1, termination `rejected`, `repairs: 0`.

Per-case (constrained): the only real pass is `denied-probe` on both splits.
`mem-roundtrip` parses worst (1/3 on both splits — the two-action case).
Everything else parses 2–3 of 3, gates, then walks to `max_steps` without
reaching its goal. Train → hold-out shows no cliff (81%→67% parse, task flat),
so the constrained failure mode generalises across the structural variation.

## Findings

**1. The harness is the existence condition, not an optimisation.** Without
the mask not one reply in 42 parses; with it, 74% do. The scaffold is what
makes the model speak the action language at all — on this model the
with/without-harness delta is not points, it is 0% → 81%.

**2. Form is necessary, not sufficient.** Both arms end at task 3/21 (and the
free arm's 3 are vacuous). The wrong-but-valid gap — parsed 31, passed 6, over
both constrained suites — is the constraint-tax signature (arXiv:2605.26128)
in our own data, and the working target for
[#124](https://github.com/geisten/geistshell/issues/124) (choice-slot
calibration) and [#125](https://github.com/geisten/geistshell/issues/125)
(reason-first scaffold).

**3. Wall time favours free only because free dies.** 22 min vs 35–45 min per
suite, but every free run is one rejected step. Time-per-*successful*-case is
the honest metric and needs [#126](https://github.com/geisten/geistshell/issues/126).

## Why free decode parses 0/21 — three compounding causes

Investigated in the source, not guessed; each cause is independently
sufficient to explain most of the zero, which is why the number is so clean.

1. **No chat framing (`template = none`).** The prompt is the raw
   s-expression context with no turn marker (src/actor/actor.c:193 applies a
   template only when the profile names one; the default is none —
   MODEL-ADAPTATION decision 3). An instruction-tuned model is never told it
   is its turn to answer *in the form*; it answers in its tuned voice
   (prose/markdown) or continues the context. The postscript already
   diagnosed framing as *the* cause of BitNet's constrained-arm parse
   failures; free decode makes Gemma fail the same way.
2. **The parser is whole-reply strict.** `spg_recommendation_parse`
   (src/actor/recommendation.c:277) parses the entire reply as one
   s-expression text whose first node must be the `(recommend …)` list. There
   is no extraction of a form embedded in prose — one word of preamble
   rejects the reply. The contract in the context *states* the schema, but a
   correct form wrapped in "Here is my recommendation:" still scores
   parse 0.
3. **eval runs with `max_repairs = 0`.** The self-repair loop
   (src/run/agent_loop.c:196) — feed the parse error back, retry — is exactly
   the mechanism a free-decoding model needs, and eval never grants it: eval's
   default is 0 while the shipped `agent` default is 2
   ([#127](https://github.com/geisten/geistshell/issues/127), finding-D
   class). Every free-arm run is one strike and out.

The constrained arm bypasses all three at once: the forced `(recommend (kind `
prefix stands in for the missing turn marker, guarantees the reply *is* the
form (satisfying the strict parser), and the mask keeps it inside the grammar.
Which is finding 1 restated mechanically — and the honest caveat: the free arm
as measured is the harshest possible free configuration. With `max_repairs 2`
(the shipped default) some free runs would plausibly recover; the 0% → 81%
lift is therefore an upper bound on the lift over a *shipped* free agent. The
suite should grow a repair-enabled free arm before the number is quoted
outside this repo.

## v1 (token-bound, diagnostic only)

gemma4-e2b, train, constrained, `tokens 512`: parse 17/21, gate 13/21, task
3/21, all budget-terminated cases at identical ~3 steps across samples. Kept
because it is the measurement that surfaced the budget artefact — and because
task-rate did not move when the budget was quadrupled, which is what licenses
reading 3/21 as the model, not the budget.

## Pending

- bitnet-large TQ2_0 (0.7B, N=10), bitnet 2B-4T I2_S (N=5), bitnet-3B I2_S
  (N=5): queued (v2b/v2c), same protocol; rows land here when done.
- qwen3 family: blocked on the engine pin (geistlib#236).
- Repair-enabled free arm ([#127](https://github.com/geisten/geistshell/issues/127)).

Raw verdicts: `~/geistshell-step4-v2/*.jsonl` on rspdevelop (JSONL per
suite×arm, driver.log for wall times).
