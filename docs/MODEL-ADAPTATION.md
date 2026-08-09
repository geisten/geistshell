# Adapting the agent to the model

geistshell drives one governed loop. The models it must drive are not
comparable: Gemma 4 E2B is instruction-tuned with its own turn tokens; BitNet
b1.58 2B-4T is ternary, fast, and **not tool-trained at all**; a frontier model
through the remote adapter plans on its own. Today the loop treats all three
identically — one raw s-expression prompt, one decoder, one set of convergence
heuristics.

This document is the design that fixes that, decided in full before
implementation. It also records what is deliberately **not** built, and why.

The governing rule: **every claim in here is a hypothesis until the baseline
suite measures it.** Steps 0–3 below build the instrument. Nothing about model
behaviour is asserted before step 4 produces a number.

## Findings that motivated this

Seven things found in the tree, in the order they bite:

| | Finding | Where |
|---|---|---|
| A | The engine pin was stale by six releases: `GEIST_REF ?= v0.2.1` against an upstream at **v0.8.2**. Ternary `I2_S` and `geist_model_arch()` arrived in 0.3.1; the agent-runtime API contract arrived in 0.6.0. No BitNet without the bump. | `Makefile:9` |
| B | Per-family chat framing exists — but **not** in geistlib. `geist_chat_template_for_model()` (auto-detects Gemma / Llama-3, which covers BitNet 2B-4T / generic) was deleted from geistlib in v0.7.0 and now lives in [geistagent](https://github.com/geisten/geistagent) `include/agent.h`. geistshell hardcodes Gemma in the chat REPL and sends the **agent** prompt with no framing at all. | `src/chat/chat_template.c:5`, `src/actor/actor.c:186` |
| C | The constant prefix (`contract` → `directive` → `goal` → `examples`) is already rendered first — and re-prefilled every tick, because `reset_model_session = true` at every call site. ~350 tokens × every step. `geist_session_pin_prefix` exists for exactly this. | `src/context/context.c:696`, `src/cli/main.c:1278` |
| D | `eval` and `agent` run **different decoders**. The `(model "geist")` case path builds the adapter with no `force_prefix` and no `capabilities` — free decode. The suite measures a configuration nobody ships. | `src/cli/main.c:2541` vs `src/cli/main.c:2034` |
| E | `--samples N` is inert on local models: temperature pinned to `0.0f`, one fixed seed, session reset per run → N byte-identical runs. The k-of-N variance story only holds for `(model "remote")`. | `src/cli/main.c:2544` |
| F | `spg_cmd_registry` is not a failed security layer. It is a **tool menu that was never wired to the prompt** — `summary` and `common_flags` are useless to the executor and meaningful only to a model. Meanwhile the `command` slot decodes completely free. | `src/exec/cmd_registry.c`, `src/model/grammar_mask.c:120` |
| G | The verifier in `--best-of N` is an oracle: `attempts = have_expect ? best_of : 1`. Without a declared expected observation the feature silently degrades to one attempt. The measured 5/5-vs-0/5 was obtained knowing the answer. | `src/cli/main.c:2156` |

## Decisions

### 1. BitNet runs in-process, on libgeist

Not through an OpenAI-compatible server. libgeist is first-party, already
supports `GEIST_DTYPE_I2_S`, and is faster than bitnet.cpp on the target
platforms. Decisive for the agent: in-process is the only path with
`peek_logits` + `prefill_tokens`, and therefore the only path where the
constrained structural decode works. The remote adapter sends no `grammar`,
no `json_schema`, no `logit_bias` — remote is free decode, which is precisely
what a tool-less model cannot do.

### 2. geistshell owns its chat-template table; geistlib owns only the key

**This decision was reversed during step 1.** The first version of this document
said the table should be promoted into geistlib's public API, on the grounds
that "which turn markers does this GGUF want" is a property of the model. That
reasoning is not wrong, but it lost to a stronger one that was already settled
upstream and that this document had not read.

geistlib `c2bf151` (v0.7.0, `feat!: geistlib becomes a pure inference engine`)
deleted the entire agent layer — 24 files, 8309 lines — into
[geistagent](https://github.com/geisten/geistagent), with the chat-template
table among them. Its stated reason applies to a second consumer exactly as it
did to the first:

> Shipping them from both repositories would have put the security boundary on
> two include paths, which is an invitation to audit one and compile the other.

So: **geistshell keeps its own ~40-line table**, taken from geistagent's
`include/agent.h`, covered by a unit test that needs no GGUF. Two copies do
drift — but they *should*: geistagent frames JSON tool calls, geistshell frames
s-expressions, and the two will want different system-turn handling.

The table gains a fourth field, `system_open` (nullable → fall back to
`user_open`, which is what Gemma needs — it has no system role).

What geistlib does owe, and now does: `docs/API_CONTRACT.md` (added in 0.6.0)
pins `peek_logits`, `pin_prefix` and `tokenize` as STABLE for exactly this
out-of-tree runtime, enforced in CI by `scripts/check-api-contract.sh` and
`examples/agent_contract_smoke.c`. That is a stronger guarantee than the
promotion this document originally asked for. The one gap was
`geist_model_arch` — the key the template is selected *by*, still
`EXPERIMENTAL` — promoted in
[geistlib#226](https://github.com/geisten/geistlib/pull/226).

The **router** from the old `tools/agent.h` is *not* copied. It selects from a
flat tool-name list; geistshell selects an `enum spg_action_kind` and then runs
a kind-dependent scaffold, which is the stronger mechanism (the bureaucratic
fields are never decoded at all). Only the **PMI calibration** is adopted — see
step 8.

### 3. Prompt framing: system turn + user turn

The constant prefix (`contract`, `directive`, `goal`, `examples`) becomes the
system turn; everything varying (`budgets`, `graph`, `memory`, `observation`,
`journal`) becomes the user turn; then `model_open`, then the forced
`(recommend (kind ` prefix.

Rejected: one combined user turn (nothing left to pin), and two consecutive
user turns (a dialogue where the user speaks twice is off-distribution for
exactly the small models this targets).

This decision fixes the `pin_prefix` boundary irreversibly, which is why it is
made now even though the pinning itself lands last.

**Default stays `template = none`** until the baseline says otherwise. Today's
measured status quo is *no framing at all*, and geistlib's own note records
that Gemma with an external SentencePiece does better on generic
`User:/Assistant:` than on its own markers — the tokenizer does not map
`<start_of_turn>` to control tokens 105/106. Framing is a measurement question,
not a design question.

### 4. The baseline suite

**Metric: a three-rung ladder, not pass/fail.** Per run: **parse-rate** (valid
form?) → **gate-rate** (policy ALLOW?) → **task-rate** (goal reached?). No new
expectation fields — `termination` plus the `reject_reason` / `deny_reason`
already carried in `spg_eval_case_result` supply all three; it is report work
in the JSONL.

The ladder is what makes the central hypothesis falsifiable: *the scaffold
lifts parse-rate to ~100% independent of the model.* If that holds, the
question collapses from "can BitNet produce structure" to "does BitNet pick the
right action" — and only the ladder shows that transition.

**Two models, always.** Gemma 4 E2B is the control group; a one-model baseline
cannot separate "BitNet is weak" from "geistshell's prompt is bad."

**Cases.** The constrained decoder makes exactly two real decisions — `kind`
and `capability`. Everything else is free text. So the suite must stress those
two, not the structure. This first requires **more than one capability per kind**
in the eval policy; `examples/policy.spg` has exactly one `local_shell`
capability today, so the capability slot is masked to a single choice and
measures nothing.

| # | Case | Goal | Tests | Verdict |
|---|---|---|---|---|
| 1 | `shell-count` | "How many lines in report.md?" | kind=local_shell, usable command | `finished` + observation contains `7` |
| 2 | `mem-recall` | "What port does staging use?" (answer is in the mind-palace) | **distractor**: a weak model reflexively reaches for shell/grep | `finished` + observation contains `8443` |
| 3 | `mem-roundtrip` | "Remember X, then tell me X" | memory_save → memory_read, two steps | `finished` + observation contains X |
| 4 | `sim-probe` | target state reachable only through the simulator | kind=simulator | `finished`, `max_steps 3` |
| 5 | `denied-probe` | "Check whether host X accepts our key" | `auth_probe.ssh_publickey_single` is `(enabled false)` — governance interaction | `denied` **or** `finished`, never a run error |
| 6 | `already-done` | answer already present in `(observation ...)` | **does the model know when to stop?** | `finished` + minimal steps |
| 7 | `cap-distractor` | a read, but `build.run` is the prior-heavy name | capability slot in isolation | `finished` + observation marker |

Case 3 replaces a plain `mem-save` case deliberately: a bare save is not
judgeable with existing machinery (the observation is a confirmation, not the
content), while the round-trip proves the save through the recall and tests
multi-step at the same time.

Case 6 needs care in judging: `finish_on_no_progress` already terminates
`FINISHED` when an observation repeats, so a model that never emits `finish`
still "passes" at 2 steps. That is exactly the heuristic decision 8 makes
measurable.

**7 + 7, train and hold-out, structurally varied** — different fixture,
different capability, same action kind. Same-shape hold-out cases measure the
train split again and turn the generalisation claim back into an assertion.
Without a hold-out split the learning gate can never judge a real model's
lesson, which is the state it is in today.

**Fixture isolation per sample.** Cases 2, 3 and 7 are stateful; case 3 writes
to memory. Run twice against one directory and run 2 finds X already there and
passes trivially without ever choosing `memory_save`. So: a `(fixture "…")`
field per case, copied into `build/eval/<case>-<sample>/` before **each
sample**, with workdir and memory-dir pointed there. A `cp -R`, not a
snapshot/restore mechanism in `mem_store`.

**`make bench`, not `make test`.** In `make test` it would need a 1.1 GB GGUF
and minutes per commit, and a fresh checkout would be red. As a purely manual
tool it rots within weeks. `make bench` reports a missing GGUF as *skipped*,
not as failure.

### 5. Model profile: an s-expression file

```
(model_profile
  (name "bitnet-full")
  (arch "bitnet-b1.58")
  (template llama3)          ; auto | none | gemma | llama3 | generic
  (constrained true)
  (command_mask true)
  (verifier ladder)          ; ladder | expect | none
  (best_of 3)
  (temperature 0.8)
  (finish_on_no_progress false))
```

Referenced from `run.spg` alongside `(policy …)` and `(scenario …)`.
Precedence: **CLI > profile file > auto-detect** (auto-detect via
`geist_model_arch()` + special-token probe, so a user with nothing but a GGUF
needs no profile).

Data, not code — the same call as the command menu in decision 7. The decisive
property is that a profile file is **versionable and journalable**: when
`make bench` emits a number, the journal must record which profile produced it,
or in three months nobody can reproduce the baseline.

**Profiles are named presets, not an orthogonal flag matrix.** Six independent
switches over 14 cases × 5 samples × 2 models is 2240 runs per measurement —
on a Pi 5 that is a weekend, not a measurement. `make bench` runs four presets
(`bitnet-conservative`, `bitnet-full`, `gemma-default`, `frontier`); the full
cross product stays reachable as a debugging tool.

`finish_on_no_progress` is a profile axis with real effect in both directions:
a strong model emits `finish` itself and is *hurt* by the heuristic (a repeated
observation is a retry, not convergence, and the run is cut short as
"finished"); a 2B model hangs without it. Suspicion to be tested at step 4:
today's heuristic inflates pass rates that are not real.

### 6. Verifier: a termination ladder, replacing the oracle

`finished` > `denied` > `rejected` > `max_steps`, tie-broken on fewer
`steps_taken`. All of it already computed per run and read by nobody. `(expect …)`
survives as an *additional* filter when present — the two do not conflict, and
a user stating "run until the output contains `BUILD OK`" is a legitimate
acceptance criterion, just not one that generalises to interesting goals.

This is what turns `--best-of N` from an eval-only trick into a shippable
feature. It also lifts the four convergence heuristics out of the loop: instead
of guessing mid-loop when a stall means success, rank N complete runs against
an explicit criterion afterwards.

### 7. The command menu, and what it is not

`spg_cmd_registry` gets wired to the two places it was built for: the model's
context (inside the **constant, pinned** prefix) and a first-token mask on the
`command` slot. It becomes loadable from a file instead of compiled in.

**No MCP.** MCP is an interop feature, not a capability feature: it does not
make the agent better at choosing the right action, it makes other people's
tools reachable. It is also where governance thins out, because the action
space then comes from a process the journal cannot replay. Two extension axes
are being conflated in the "a new tool is a code change" complaint:

- New action **kinds** are genuinely a code change — enum, scaffold, executor,
  policy — **and that is correct**. The per-kind scaffold is the mechanism that
  makes tool-less models work; a scaffold generated at runtime from an MCP
  schema is the same structure, untested and no longer byte-replayable.
- New **tools inside `local_shell`** are not a code change and never were. The
  action space is already open. What is missing is not executability but
  *discoverability for the model* — a table row.

Revisit only if geistshell becomes a product for third parties bringing their
own tools.

**The mask is model-side only. The executor is unchanged.** Three consequences,
without which this becomes exactly the layer that looks like security and is
not:

1. **Rename it.** `spg_cmd_registry` → a name that says *menu*, not *registry*.
   A name that sounds like access control is itself the bug.
2. **The menu file is untrusted.** Once loadable it is a *model input*, in the
   same class as scenario and corpus text. It may influence what the model
   **proposes** and never what the executor **permits**. If it ever did both,
   editing a data row would be privilege escalation.
3. **Say it in the header.** "This is a proposal space for the model; the
   boundary is `executor_boundary` + rlimits + capability."

A name allowlist in the executor was rejected: it only buys real security if
shell metacharacters, `sh -c`, path invocations and interpreters are banned too
— at which point `local_shell` is gone, and with it the reason geistshell is
not geistagent. A half allowlist is worse than none, because it gets believed.

### 8. Planner and embeddings: named seams, not flags

Configurability is free for switches and expensive for subsystems.
`template`, `constrained`, `command_mask`, `verifier`, `best_of`, `temperature`,
`finish_on_no_progress` are switches over code that exists or costs ~15 lines.
`planner = on|off` is the opposite: the flag does not make the planner cheaper,
it makes it *more* expensive, because both paths then have to work and be
tested. A feature flag is not a discount on the feature.

What does make both cheap is that they sit on seams that already exist:

- **Planner = a new action kind, not a new loop layer.** `(recommend (kind plan)
  (steps "…"))` passes through the policy gate, the journal and replay like any
  other action, gets its scaffold, and costs one enum member plus an executor
  that writes the sub-goals into the context. No spine surgery. A model that
  cannot plan simply never picks the kind — and under constrained decoding it
  can be switched off for BitNet with `(enabled false)` in the policy. **The
  planner is a policy line, not a config field.**
- **Embeddings = a different ranker behind `spg_mem_store`.** The retrieval
  interface stands; what changes is a scoring function.
- **Streaming.** The constrained decode path is already token-by-token
  (`src/model/model_adapter.c:206`); streaming is a callback, not a rewrite.
  Zero effect on agent quality — it is chat-REPL UX. An hour's work when a human
  is watching, otherwise YAGNI.

Both land when step 4 shows **which rung of the ladder actually breaks**. A
profile field for an executor that does not exist is configuration for a value
that never changes.

The embeddings question has a cheap experiment attached: the same case with
good and with sabotaged retrieval. No difference → settled. At 4k context only
a handful of facts fit anyway, and keyword+recency vs. embeddings over ~50 facts
at top-5 rarely diverges.

## Sequence

Each step is separately measurable or trivial.

| # | Step | Why here |
|---|---|---|
| 0 | `GEIST_REF` 0.2.1 → 0.8.2 *(done)* | No `I2_S`, no BitNet. Not one line — see below. |
| 1 | libgeist: `geist_model_arch` → STABLE *(PR open)*. geistshell: copy the template table from geistagent, add `system_open` — lands with step 5 | Blocks everything model-specific. |
| 2 | `eval` and `agent` onto the **same** decoder (finding D); `--temperature` for `eval`/`improve` (finding E) *(done)* | Otherwise the baseline measures a configuration nobody runs. |
| 3 | Per-sample fixture isolation + `make bench` *(done)* | Otherwise every number from cases 2/3/7 is a lie. |
| 4 | **Baseline: 7+7 cases, ladder metric, BitNet + Gemma, presets** | The zero point. Everything after is measured against it. |
| 5 | Model profile as s-expression, incl. the `finish_on_no_progress` axis | First hypothesis tested against the zero point. |
| 6 | Verifier ladder replaces the `have_expect` oracle | Makes `--best-of` honest. |
| 7 | Command menu → prompt + `command` mask (with the three consequences above) | Second hypothesis. |
| 8 | PMI calibration in `decode_choice_slot` | Third hypothesis. |
| 9 | `pin_prefix` for the constant prefix | Pure speed, changes no numbers — hence last. |

Steps 0–3 are instrument-building with no insight of their own. That is the
price of every number from step 4 onward meaning something.

### Step 0, as it actually went

The pin bump was expected to be one line. It was not, and what it exposed
belongs in the record.

v0.3.1 moved `heap.h` from the engine's repo root to `src/base/heap.h`, and the
build broke — because `CPPFLAGS` carried `-I$(GEIST_DIR)`, the engine's **repo
root**, so geistshell had been including a *private* engine header all along.
`include/geistshell/allocator.h` wrapped `memory_arena` from it, and
`src/cli/main.c` called `heap_alloc_aligned` / `safe_free` directly.

Rather than add an include path for the new location, the coupling was removed:

- `spg_arena` (`allocator.{c,h}`) was used by **no production code** — only by
  its own test — which is consistent with the repo's allocation-free,
  caller-provided-buffer constraint. Deleted, along with its test and its entry
  in `geistshell.h`.
- `main.c`'s two `heap_alloc_aligned(n, alignof(char))` calls became `malloc(n)`
  (identical semantics for char alignment), and its nine `safe_free` calls
  became a four-line local `free_ptr`. The CLI already mixed plain `free()` with
  these, so nothing changed behaviourally.
- `-I$(GEIST_DIR)` was dropped from `CPPFLAGS`. Only `-I$(GEIST_DIR)/include`
  remains, so a future private-header dependency fails at compile time instead
  of working by accident.

Net: 2 files deleted, ~60 lines removed, one cross-repo coupling gone. Full
`make test` green under ASan/UBSan.

The lesson generalises: the engine boundary was never enforced by the build, so
it drifted.

**And the bump itself was wrong the first time.** It went to `v0.3.1` — the tag
that happened to be checked out in `deps/geist`, mistaken for the upstream
state. Upstream was at **v0.8.2**, six releases further on, including the
0.6.0 API contract and the 0.7.0 agent-layer removal that reversed decision 2
above. Corrected to `v0.8.2`; `make test` green.

Two failures with one shape: a local artefact was read as the state of the
world. The build directory said which engine version existed; `deps/geist`
said which one upstream had. Neither was a source of truth, and neither
announced that it wasn't.

## What is not on the list

Planner, embeddings, streaming, MCP, Ed25519 journal signatures. Not because
they are wrong — because none of them can be justified before step 4 says where
it actually breaks. Adding any of them now is optimising against a feeling,
which is the exact condition the learning gate has been in since it was built.
