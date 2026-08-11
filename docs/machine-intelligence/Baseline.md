# Baseline — Phase 0

Eingefrorener Ausgangszustand für die Machine-Intelligence-Roadmap (Tracking:
geisten/geistshell#78).

Dieses Dokument enthält **nur das Delta** zu vorhandener Dokumentation. Es
wiederholt nicht, was bereits geschrieben steht:

| Frage | Steht in |
|-------|----------|
| Architektur, die drei Loops, Layout | [README.md](../../README.md) — `## Architecture`, `### The three loops`, `## Layout` |
| C23-Regeln, Buffer-Contracts, Allokation, Teststil | [.agent/AGENT.md](../../.agent/AGENT.md) |
| Eval-Harness, Reproducible benchmark | [README.md](../../README.md) — `### Evaluation`, `#### Reproducible benchmark` |
| Lernen aus echten Läufen | [docs/LEARNING.md](../LEARNING.md) |
| Modell-Adaption, Profile, Decoder | [docs/MODEL-ADAPTATION.md](../MODEL-ADAPTATION.md) |

Neu ist hier: das **Determinismus-Inventar**, die **Extension Points** mit
Dateiverweisen, der **Freeze** und die **Befunde**.

---

## 1. Der Freeze

Eingefroren wird kein Zeitwert und keine Binärgröße, sondern die einzige
Eigenschaft, die alle 17 Phasen überleben muss: **byte-identisches Journal aus
einem scripted Lauf.**

```
sha256(build/tick-demo.sgj) = 8b831838c4c21eb6f11903d7ece6013546e67ef89bccd7f1a4350cc077761c65
```

Erzeugt von `geistshell run --config examples/run.spg --fake '(recommend (kind
simulator) ...)' --ticks 1` — dem Szenario, das `test/test_cli_replay.sh`
ohnehin fährt (8 Records). Zwei aufeinanderfolgende Läufe wurden verglichen und
sind byte-identisch.

Geprüft von [`test/test_cli_baseline.sh`](../../test/test_cli_baseline.sh).

**Regel bei Bruch:** Der Test ist ein harter Fehlschlag, kein Hinweis. Die
Konstante darf nur in einem Commit aktualisiert werden, dessen Nachricht die
Änderung benennt, die die Journal-Bytes legitim verändert hat. Ein reflexhaft
aktualisierter Hash ist wertlos.

Der Golden Run läuft über den **Fake-Adapter** und ist damit
engine-unabhängig — ein späterer libgeist-Bump darf ihn nicht brechen.

### Beiwerk (maschinenabhängig, keine Assertion)

| Größe | Wert bei Freeze |
|-------|-----------------|
| C-Testprogramme | 39 |
| CLI-Testskripte | 21 (inkl. dieses Freeze-Tests) |
| `make test` | grün, exit 0 |
| Engine-Pin | libgeist `v0.8.2` |
| Toolchain beim Freeze | clang, macOS arm64, `-fsanitize=address,undefined` |

Build- und Laufzeiten sind bewusst **nicht** eingefroren: sie sind
maschinenabhängig und taugen nicht als Regressionsanker.

---

## 2. Determinismus-Inventar

Das ist die kritischste Eigenschaft für alle folgenden Phasen, weil Telemetrie
per Definition zeitvariant ist.

### Zeit wird injiziert, nie gelesen

`spg_journal_writer_append()` nimmt `timestamp_ns` als Parameter
([journal.h:74](../../include/geistshell/journal.h)); das Journal-Modul liest
selbst nie eine Uhr. Die CLI reicht einen synthetischen Tick-Zähler ein:
`timestamp_ns = 1` bzw. `i + 1u` ([main.c:1273](../../src/cli/main.c),
[main.c:1568](../../src/cli/main.c)).

Der Zeitwert geht über `hash_record()` in die Hash-Chain ein
([journal.c:161](../../src/journal/journal.c)) — eine echte Uhr an dieser Stelle
würde Replay-Byte-Identität sofort zerstören.

### Der einzige Clock-Read der Runtime

`now_ns()` in [cmd_executor.c:50](../../src/exec/cmd_executor.c),
`CLOCK_MONOTONIC`, ausschließlich für Kommando-Timeouts. Der Wert verlässt das
Modul nicht und erreicht weder Context noch Journal.

**Konsequenz für Phase 1 (#61):** Die Telemetrie-API bekommt `timestamp_ns`
vom Aufrufer und liest keine Uhr. Δt für CPU-Utilization wird aus zwei
injizierten Zeitstempeln gebildet, nicht aus `clock_gettime`. Andernfalls
entsteht eine zweite Zeitquelle und der Freeze oben bricht.

### Offene Determinismus-Risiken für die Folgephasen

- Reihenfolge der OS-Prozessliste (`readdir` über `/proc`) ist nicht garantiert
  — Phase 2/3 müssen explizit sortieren.
- Counter-Rücksprünge (Suspend/Resume) erzeugen negative Deltas.
- Erster Tick hat kein Δt: CPU-Utilization ist dort `unknown`, nicht `0`.

---

## 3. Extension Points

| # | Zweck | Ort | Art |
|---|-------|-----|-----|
| 1 | Telemetrie-Modul | **neu**: `src/machine/`, `include/geistshell/machine_state.h` | additiv |
| 2 | Action Kinds | `enum spg_action_kind` — [policy.h:17](../../include/geistshell/policy.h) | additiv (Enum-Erweiterung) |
| 3 | Capability-Auflösung | `resolve_capability_span()` — [policy_gate.c:23](../../src/policy/policy_gate.c), Gate-Eintritt [policy_gate.c:197](../../src/policy/policy_gate.c) | additiv |
| 4 | Budget-Prüfung | `would_exceed()` — [policy.c:189](../../src/policy/policy.c) | unverändert nutzbar |
| 5 | Action-Dispatch | [orchestrator.c:237–315](../../src/run/orchestrator.c) (`FINISH`, `MEMORY_*`, `LOCAL_SHELL`) | invasiv: jeder neue Kind braucht hier einen Zweig |
| 6 | Recommendation-Parser | `switch (out->action_kind)` — [recommendation.c:147](../../src/actor/recommendation.c) | invasiv |
| 7 | Context-Block | `spg_context_build()` / `spg_context_render()` — [context.c:270](../../src/context/context.c), [context.c:678](../../src/context/context.c) | additiv |
| 8 | Journal-Event | `enum spg_journal_event_kind` — [journal.h:20](../../include/geistshell/journal.h) | `SPG_JOURNAL_EVENT_ACTION` genügt zunächst |

### Warum `src/machine/` und nicht `host_probe`

`spg_host_probe()` ([host_probe.h:38](../../include/geistshell/host_probe.h))
liefert **statische Host-Identität** aus `uname(2)`: fünf Textfelder, einmal
gelesen, genau ein Aufrufer
([exec_command.c:91](../../src/exec/exec_command.c)), kein Zeitbezug.

Telemetrie ist das Gegenteil: zeitvariant, gesampelt, mit Δt-Zustand über
Ticks. Beides in einem Modul zu führen heißt, jedem `host_probe`-Aufrufer die
Sampling-Kosten aufzuerlegen und zwei Lebenszyklen zu vermischen. Deshalb neues
Modul; `host_probe` bleibt unverändert.

---

## 4. Befunde

### 4.1 Main hing sechs Commits zurück (behoben in diesem PR)

`claude/geistshell-optimization-agents-2ae297` trug +2140 Zeilen, die nie in
main waren:

| Commit | Inhalt | Issue-Status vorher |
|--------|--------|---------------------|
| `cb5e2a4`, `3d6fe5a` | Engine-Pin v0.3.1 → v0.8.2, `allocator.h` entfernt | #59 Schritt 0 als erledigt geführt |
| `2aa0cb8` | `docs/MODEL-ADAPTATION.md` | von #54, #55, #59 verlinkt, existierte nicht |
| `5462b03` | eval/agent gleicher Decoder | #51 geschlossen |
| `02575a4` | Fixture-Isolation, `make bench` | #52 geschlossen |
| `8e46e0c` | Baseline-Suite 7+7, parse/gate/task-Leiter | #53 offen, obwohl gebaut |

Zwei geschlossene Issues ohne Code in main, ein offenes Issue mit fertigem
Code. Ohne diesen Merge hätte Phase 0 einen Zustand eingefroren, an dem niemand
arbeitet — und Phase 4/10 hätten die Leiter aus #53 vermutlich ein zweites Mal
gebaut.

**Folgeaktion:** #53 nach dem Merge schließen, #59 Schritt-Status korrigieren.

### 4.2 Ohne `make sync-engine` baut das Repo nicht

`deps/geist` ist nicht vendored, sondern wird per `GEIST_REF` geklont. Der
Freeze-Hash ist davon unabhängig (Fake-Adapter), der Build nicht. Für jeden
reproduzierten Messwert gehört der Engine-Ref in den Bericht.

### 4.3 Neue Action Kinds sind nicht rein additiv

Extension Points 5 und 6 verlangen für jeden neuen Kind einen Zweig im
Orchestrator und im Parser. Das ist für Phase 6 (#66) einkalkuliert, aber es ist
der Punkt, an dem Phase 15 (#75) misst, ob geistshell Plattform oder
projektspezifisches Framework ist: braucht Machine B dort erneut Zweige, ist die
Warnung aus #75 eingetreten.

---

## 5. Minimalste Änderung für Phase 1

Ein neues Modul, kein Eingriff in bestehende Pfade:

```
include/geistshell/machine_state.h   struct spg_machine_state, fixed capacity
src/machine/telemetry.c              reine Parser über Puffer, kein I/O
src/machine/telemetry_linux.c        /proc- und /sys-Lesen, isoliert
test/test_machine_telemetry.c        statische Fixtures
```

Signaturform nach `.agent/AGENT.md` (Länge vor Array):

```c
[[nodiscard]] enum spg_status
spg_telemetry_parse_proc_stat(size_t n, const char buf[static n],
                              struct spg_cpu_sample *out);

[[nodiscard]] enum spg_status
spg_machine_sample(uint64_t timestamp_ns, const struct spg_cpu_sample *prev,
                   const struct spg_cpu_sample *cur,
                   struct spg_machine_state *out);
```

Kein Aufruf im Agent-Pfad in Phase 1 — die Verdrahtung in den Context passiert
erst in Phase 3 (#63).


## Konstanten-Historie

Der Anker wird nur mit Begründung aktualisiert; jede Änderung steht hier.

| Datum | Hash | Warum |
|---|---|---|
| Phase 0 | `52bd794e…37365` | Ausgangswert |
| #56 | `8b831838…761c65` | Das Kommando-Menü wird als `(tools ...)` in den Kontext gerendert. Der `model_input`-Payload wächst um einen konstanten Block, also verschiebt sich die Hash-Kette. Der Lauf bleibt deterministisch — zwei Läufe hintereinander liefern denselben Hash; er enthält nur mehr. |

Eine Eigenschaft, die dabei neu zu wahren ist: das gerenderte Menü darf **nicht
vom Host-OS abhängen**, sonst wird dieser Hash plattformabhängig und der
Determinismus-Kanarienvogel hört auf, auf der Maschine einer zu sein, die ihn
nicht ausführt. Alle eingebauten Einträge sind `SPG_CMD_OS_ALL`;
`test_cmd_menu.c::test_render_is_host_independent` prüft das, damit die erste
plattformspezifische Zeile laut auffällt statt still.
