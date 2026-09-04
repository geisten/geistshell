# Machine Eval MVP — was schon gebaut ist

Abgleich eines externen 8-Wochen-MVP-Plans gegen den Stand des Repos.

Das Ergebnis vorweg, weil es die Reihenfolge aller Arbeit ändert: **die Wochen 1
bis 7 dieses Plans entsprechen den Phasen 1 bis 17 und sind gelaufen.** Wer den
Plan als Arbeitsliste liest, baut sechs Wochen lang etwas, das im Repo steht.

Dieses Dokument hält fest, welcher Abschnitt wo erledigt ist, was gestrichen
gehört und was tatsächlich fehlt.

## Der Abgleich

| Plan | Im Repo | |
|---|---|---|
| §4 Machine State (W1) | `machine_state.h`, `backend_linux.c`, `backend_macos.c`, [Baseline.md](Baseline.md) | ✅ |
| §5 Managed Processes (W2) | `spg_process_profile`, [Processes.md](Processes.md), `examples/machine/profiles/` | ✅ |
| §6 Typed Actions (W4) | `SPG_ACTION_MACHINE_PAUSE`/`_RESUME`, [Actions.md](Actions.md) | ✅ |
| §8 Reale Workloads | `examples/machine/workloads/`, [Workloads.md](Workloads.md) — Phase 8 | ✅ |
| §9 Messbare Goals (W5) | `machine_after`, `machine_goal`, [Closed-Loop.md](Closed-Loop.md) — Phase 9 | ✅ |
| §10 Policy und Safety | `SPG_POLICY_DENY_UNMANAGED_PROCESS`, `_PROCESS_PROTECTED`, `_PROCESS_IDENTITY` | ✅ |
| §11 Metriken | Latenz je Case, peak RSS — Phase 10, `src/cli/main.c` | ✅ |
| §12 Rule Baseline (W3) | `examples/eval/machine/diagnosis_rules.spg`, `examples/machine/rule_sensitivity.c` | ✅ |
| §13 Model Comparison (W7) | [Model-Tournament.md](Model-Tournament.md) — Phase 10, `run_tournament.sh` | ✅ |
| §15 Regression Testing | `improve --validate`, Hold-out-Suiten | ✅ |
| §16 Journal und Replay | Hash-Kette, logische Zeitstempel, byteidentischer Replay | ✅ |
| §17 Security Suite (10 Punkte) | [Security-Review.md](Security-Review.md) — Phase 16, **12** Angriffe | ✅ 10 von 12 |
| §7 „mindestens 100 Szenarien" | 14 States, rund 9 Cases | ❌ |
| §14 `machine init`, `machine.yaml` | `machine`-Subkommando vorhanden, `init` und YAML nicht | ⚠️ |

Zwei der zwölf Angriffe aus Phase 16 stehen dort ausdrücklich als offenes
Restrisiko: die manipulierte Memory-Lesson (7) und der Replay mit verändertem
Host-State (11). Angriff 9 — der Parser kennt keine Obergrenze für `reason` —
ist teilweise offen.

## Die Frage nach der Rule Baseline ist beantwortet — und war die falsche

Plan §19, Woche 3 fragt: *„Liefert AI gegenüber Regeln relevanten Zusatznutzen?"*
Phase 4 und 5 haben sie gemessen ([Diagnosis-Benchmark.md](Diagnosis-Benchmark.md),
[Small-Model-Gap.md](Small-Model-Gap.md)):

| Methode | Known | Held-out | Zeit |
|---|---|---|---|
| **Regeln** | **6/6** | **3/3** | < 1 ms |
| Gemma4-E2B Q4_K_M | 2/6 | 1/3 | 253 s |
| BitNet b1.58-3B | 0/6 | 0/3 | 388 s |
| BitNet b1.58-large | 0/6 | 0/3 | 89 s |

**Die Antwort steht, und sie wird sich nicht drehen.** Neun Szenarien, ein
geschlossener Ursachenraum, ein eingefrorener Snapshot rein, eine Klassifikation
raus: vier Schwellwertvergleiche sind dafür die natürliche Form, und kein Modell
wird sie darin schlagen. Wer diese Tabelle als Produktentscheidung liest, hat den
Vergleich gewonnen und die Aufgabe verfehlt.

Denn die Regeln in dieser Zeile gibt es nur, weil jemand sie geschrieben hat —
für **diese** neun Szenarien, für **diese** Maschine, mit Schwellwerten, die den
Sensitivitätslauf nach unten überstehen und nach oben auf 6/9 einbrechen. Das ist
die eigentliche Aussage der Tabelle: eine Rule Baseline ist kein Konkurrent, sie
ist ein **Maßstab für die Fälle, die schon jemand verstanden hat**.

### Wofür geistshell antritt

> In einer Umgebung, für die niemand Regeln geschrieben hat, zurechtkommen und
> das System **kontrollieren** — notfalls mit selbst erstellten Regeln. Die
> Kontrolle bleibt bei uns.

Daraus folgt, was gemessen wird und was nicht:

| Nicht die Aufgabe | Die Aufgabe |
|---|---|
| Einen Schwellwertvergleich schlagen | Handeln, wo kein Schwellwert existiert |
| Diagnose bei geschlossenem Ursachenraum | Ein Ziel verfolgen, das sich im Lauf ändert |
| Der beste Regler sein | Entscheiden, wann geregelt und wann angehalten wird |
| Eine Regel je Fall vorab | Eine Regel aus dem Lauf gewinnen, durch das Gate, ins Journal |

Die Governance ist dabei die nicht verhandelbare Hälfte: eine selbst gewonnene
Regel ist genau so viel wert wie das Gate, das sie prüft, und das Journal, das
sie nachvollziehbar macht. Ein Agent, der sich seine eigenen Regeln gibt, ist
ohne Policy Gate kein Produkt, sondern ein Risiko.

Deshalb ist der PID-Vergleich in Woche 2 unten so gebaut, dass der Sollwertfall
dem Regler gehört — und die Verriegelung, der Zielwechsel in Worten und der
widersprüchliche Sensor die Fälle sind, für die niemand vorher eine Regel hatte.

## Die Lücke zwischen Plan und Repo

Der Plan enthält **kein Wort über Aktuatorik**. „Machine" heißt darin
durchgehend: die Prozesse des Referenzrechners selbst, gesteuert über
`SIGSTOP`/`SIGCONT`. Es gibt darin keinen Kanal, kein `device_write`, keine
Anlage.

Das Repo hat seit der Geräte-Arbeit eine zweite Achse: eine Kanaltabelle, eine
nicht umkehrbare Aktion und drei fremde Maschinen (siehe [Devices.md](Devices.md)).
Das sind zwei verschiedene Produkte, und der Plan kennt nur das erste —
dasjenige, dessen Messung bereits negativ ausgefallen ist.

## Was gestrichen wird

| Streichen | Warum |
|---|---|
| §19 Wochen 1–5 und 7 | Gebaut und dokumentiert. Sechs von acht Wochen |
| §7 „mindestens 100 Szenarien" | Anzahl ist eine Eitelkeitsmetrik. Der Sensitivitätslauf hat mehr gelehrt als 86 weitere Szenarien es täten. Stattdessen rund 30 entlang benannter Dimensionen — und Sensitivität als eigene Dimension |
| BitNet aus der laufenden Suite | 0/6 und 0/3 bei 388 s bzw. 89 s. Das archivierte Ergebnis bleibt, der Lauf kostet Zeit für eine bekannte Null |
| §5 YAML-Profil, §14 `machine.yaml` | Das Repo spricht sexpr — Policy, Suiten, Simulator, Fixtures und die Modellausgabe. Eine zweite Konfigurationssprache ist der Preis, nicht die Klammern |
| §25 „Quality vs Model Size" als offene Frage | Für Diagnose beantwortet, `run_size_curve.sh` existiert, die Kurve endet bei null. Nur sinnvoll, wenn auf Regelung neu gestellt |
| §4 „optional Leistungsaufnahme, Lüfter" | Der Lüfter-Port ist schon einmal gestorben: drei Backends, je zwei Funktionen, kein Aufrufer. Nicht wiederbeleben |
| §21–§24 Marktvalidierung, OSS-Tiers | Kein Code. Nicht falsch, aber kein Arbeitspaket dieses Dokuments |

Von 26 Abschnitten bleiben etwa sechs.

## Was bleibt: drei Wochen

### Woche 1 — den Auditbeweis vollständig machen

*„Every run is auditable and replayable"* hatte auf einer Anlage ein Loch:
Sensorwerte wurden weder abgetastet noch journalisiert. Ein Beleg, der zeigt,
was der Agent tat, aber nicht, was er sah, ist bei einer nicht umkehrbaren
Aktion der halbe Beleg.

**Der separate Journalschritt entfällt.** Der Actor journalisiert unter
`SPG_JOURNAL_EVENT_MODEL_INPUT` den kompletten gerenderten Kontext. Steht der
Block im Kontext, steht er in der Hash-Kette — kein neues Event-Kind, keine
Migration eingefrorener Fixtures. Abtasten und rendern ist ein Schritt, nicht
zwei.

- `examples/plant-policy.spg` ist die Policy dafür: eine Capability, sonst nichts

**Deliverable:** ein Anlagenlauf, dessen Replay die Eingaben der Entscheidung
enthält. **Erledigt** — `test_cli_device.sh` prüft, dass der Kontext vor dem
Schreiben `(heater 0)` trägt und der danach `(heater 40)`.

### Woche 2 — die Anlagen-Eval mit PID-Baseline

Das Stück, das im Plan fehlt und über die Positionierung entscheidet.

- `plant_after` als Bewertungsachse, analog zu `machine_after`
- Baseline: ein PID-Regler in derselben Tabelle, so wie die Regeln beim
  Diagnose-Benchmark
- **Der Sollwertfall allein misst nichts.** Ein PID schlägt ein Sprachmodell beim
  Halten von 60 °C vernichtend. Eine Eval, die nur das prüft, widerlegt das
  eigene Produkt ein zweites Mal, ohne dass es nötig wäre

Die Fälle, in denen ein Regler keine Antwort hat:

| Fall | Warum der PID scheitert |
|---|---|
| Verriegelung eingerastet | Ein PID kennt keinen Zustand „gesperrt". Zurücksetzen oder nicht ist eine Entscheidung mit Folgen |
| Ziel wechselt im Lauf, in Worten | „ab jetzt sparsam heizen" — ein PID hat einen Sollwert, keine Absicht |
| Sensor `unknown`, Trend widersprüchlich | Ein PID regelt weiter gegen eine Zahl, die nicht mehr gilt |

Der Sollwertfall gehört trotzdem hinein — als der, den der PID gewinnt, sonst
ist die Tabelle unglaubwürdig.

**Deliverable:** die erste Zahl zur Frage, ob ein kleines Modell eine Anlage
fahren kann.

### Woche 3 — Bericht und die offenen Angriffe

- Angriff 7 und 11 aus [Security-Review.md](Security-Review.md) schließen,
  Angriff 9 ist ein Parser-Limit
- Bericht mit dem PID daneben, veröffentlicht auch wenn er verliert

**Deliverable:** die erste Messung auf der Aufgabe, für die geistshell antritt —
Regelung und Entscheidung statt Diagnose.

## Der Entscheidungspunkt danach

| Ergebnis | Was folgt |
|---|---|
| Agent schlägt den PID bei Verriegelung und Zielwechsel | Die Positionierung trägt. Dann die 30 Szenarien, dann der exec-Transport |
| Agent verliert überall | Zweites negatives Ergebnis. §20 PIVOT wird sehr schwer zu umgehen |
| PID gewinnt den Sollwert, Agent die Entscheidungsfälle | Das wahrscheinlichste und das interessanteste: das Produkt ist dann *der Agent entscheidet, der Regler regelt*, und die Kanaltabelle ist die Grenze dazwischen |

Der exec-Transport aus [Devices.md](Devices.md) steht bewusst **hinter** allen
dreien. Er ändert an keiner Messung etwas — `heater.py` spricht Modbus, und das
läuft.
