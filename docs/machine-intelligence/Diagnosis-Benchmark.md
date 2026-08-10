# Diagnosis Benchmark — Phase 4

Kann ein Modell aus Machine State und Prozess-Semantik eine brauchbare Diagnose
stellen? Diese Phase misst das — **bevor** irgendeine schreibende Aktion
existiert.

Ticket: geisten/geistshell#64. Voraussetzung: [Context.md](Context.md).
Die Regel-Baseline, gegen die sich alles messen lassen muss, ist Phase 5 (#65).

## Die Diagnose ist der `finish`-Reason

```
(recommend (kind finish) (reason "<kategorie> [<prozess-id>]"))
```

Kein neuer Action Kind, kein Executor, keine Capability. `finish` trägt keine
Capability, verbraucht kein Budget und erreicht den Policy Gate nie — der
Reason **ist** die Ausgabe und wird wie jede andere journaled.

Das Ticket ließ offen, ob ein eigener side-effect-freier Diagnose-Output
sauberer wäre. Ist er nicht: ein zweiter Ausgabekanal bräuchte Grammatik,
Parser, Journal-Event und Test — für eine Information, die in ein bestehendes
Feld passt. Wenn Phase 9 strukturierte Ziele einführt, ist das der Moment, das
erneut zu prüfen.

## Kategorien sind ein geschlossener Satz

`healthy`, `batch_pressure`, `critical_pressure`, `memory_pressure`,
`thermal_anomaly`, `inconclusive`.

Freitext zu bewerten hieße messen, wie der Bewerter fühlt, nicht was das Modell
geschlossen hat. Eine Antwort außerhalb des Satzes zählt als **keine** Diagnose
— nicht als falsche.

## Die Szenarien

| # | Fall | Erwartet | Was er prüft |
|---|------|----------|--------------|
| 1 | CPU-Last durch Batch-Job | `batch_pressure` | der Lehrbuchfall |
| 2 | Gleiche Last, Verursacher ist kritisch | `critical_pressure` | liest das Modell **wer**, oder nur „CPU hoch"? |
| 3 | Speicherdruck, dominanter RSS beim Batch | `memory_pressure` | anderer Ressourcentyp |
| 4 | Heiß bei mäßiger Last, Firmware drosselt | `thermal_anomaly` | Last erklärt die Temperatur nicht — die Ursache ist kein Prozess |
| 5 | Widersprüchliche und fehlende Telemetrie | `inconclusive` | darf das Modell „weiß nicht" sagen? |
| 6 | Normalzustand | `healthy` | erfindet es auf dem leichtesten Input eine Ursache? |
| 7 | **held-out**: Batch heiß **und** Drosselung | `thermal_anomaly` | zwei Signale, keine Trainingskombination |
| 8 | **held-out**: Speicherdruck durch den **kritischen** Prozess | `memory_pressure` | die sichere Abhilfe existiert nicht, die Diagnose muss trotzdem stimmen |
| 9 | **held-out**: nur Systemtelemetrie, keine Prozessliste | `inconclusive` | hohe Last ohne Zurechenbarkeit |

Known und held-out werden **getrennt** berichtet, nie gemittelt. Ein
Durchschnitt lässt eine Suite stark aussehen, die nur die gezeigten Fälle
gelernt hat.

Die Szenarien liegen als Machine-State-Fixtures unter
`examples/eval/machine/states/`. Ein Fixture ist derselbe Block, den der
Renderer erzeugt — ein Roundtrip-Test (rendern → parsen → rendern,
byte-identisch) hält beide Hälften des Formats zusammen.

## Was gemessen wird

| Metrik | Woraus |
|---|---|
| korrekte Kategorie | Reason gegen die Erwartung, known und held-out getrennt |
| Halluzination | der Reason nennt eine Prozess-ID, die im Snapshot nicht vorkommt |
| unnötige Action | der Lauf brauchte mehr als einen Schritt, also hat das Modell etwas **getan** |
| Parse-/Reject-Rate | die parse/gate/task-Leiter aus #53, unverändert übernommen |
| Context-Größe | Bytes des gerenderten `(machine-state ...)`-Blocks je Szenario |

Kein Grader, kein zweites Modell, das das erste bewertet. Jede Zahl stammt aus
dem, was der Lauf tatsächlich emittiert hat.

**Eine falsche Kategorie lässt den Case scheitern**, nicht nur eine Notiz
erzeugen. Sauber zu terminieren ist nicht dasselbe wie recht zu haben; sonst
meldete die Kopfzahl der Suite eine Übereinstimmung, die sie nie gemessen hat.

## Die Suite prüft sich selbst

9/9 auf der Ground-Truth-Suite beweist nichts — dieselbe Zahl käme heraus, wenn
die Metrik fest auf „korrekt" verdrahtet wäre. Deshalb gibt es
`diagnosis_negative.spg`: dieselben Zustände, absichtlich falsch beantwortet.

| Negativfall | Erwartet | Gemessen |
|---|---|---|
| falsche Kategorie | Case scheitert | `correct=0`, `outcome=fail_observation` |
| erfundener Prozess `ghost_job` bei richtiger Kategorie | erkannt | `hallucinated=1` |
| Aktion vorgeschlagen statt nur diagnostiziert | gezählt | `action_proposed=1`, `steps=2` |
| `batch_pressure [critical_app, batch_job]` (echte Gemma-Antwort) | **keine** Halluzination | `hallucinated=0` |
| `healthy))` (echte Gemma-Antwort) | Kategorie erkannt | `emitted=healthy` |

`test/test_cli_diagnosis.sh` fährt beide Suiten und schlägt fehl, wenn der
Harness einen dieser Fälle durchgehen lässt. Er ist Teil von `make test`.

## Ergebnisse

### Scripted Ground Truth (deterministisch, in `make test`)

| Methode | Known | Held-out | Halluzination | Unnötige Action | Parse |
|---|---|---|---|---|---|
| scripted baseline | 6/6 | 3/3 | 0 | 0 | 9/9 |

Das ist keine Modellleistung, sondern der Nachweis, dass Harness und Metriken
funktionieren.

### Gemma4-E2B, lokal auf dem Pi 5

`--constrained --samples 1`, Release-Build, aarch64.

| Methode | Known | Held-out | Halluzination | Unnötige Action | Parse | Gate |
|---|---|---|---|---|---|---|
| Gemma4-E2B (Q4_K_M) | 2/6 | 1/3 | 0 | 0 | 9/9 | 9/9 |

Die Antworten im Wortlaut:

| Szenario | Erwartet | Geantwortet |
|---|---|---|
| batch_cpu | batch_pressure | `memory_pressure [critical_app, batch_job]` |
| critical_cpu | critical_pressure | `critical_pressure [critical_app]` ✓ |
| memory_batch | memory_pressure | `memory_pressure [critical_app, batch_job]` ✓ |
| thermal | thermal_anomaly | `memory_pressure [critical_app, batch_job]` |
| contradictory | inconclusive | `healthy))` |
| healthy | healthy | `critical_pressure [critical_app]` |
| heldout_thermal_batch | thermal_anomaly | `memory_pressure [critical_app, batch_job]` |
| heldout_memory_critical | memory_pressure | `memory_pressure [critical_app, batch_job]` ✓ |
| no_processes | inconclusive | `memory_pressure [unknown]` |

**Das Modell diagnostiziert nicht, es rät ein Label.** In sechs von neun Fällen
lautet die Antwort `memory_pressure`, unabhängig davon, ob Speicher überhaupt
knapp ist — bei `thermal` sind 4,2 GB installiert und 1,1 GB belegt. Die
Kategorie `thermal_anomaly` kam kein einziges Mal vor, obwohl zwei Szenarien sie
verlangen und die Temperatur als eigenes Feld im Context steht.

Der Vergleich, der das einordnet: wer **immer** `memory_pressure` antwortet,
trifft 1/6 known und 1/3 held-out, also 2/9. Gemma erreicht 3/9. Der Abstand zu
einer konstanten Antwort ist ein einziger Fall — und der (`critical_cpu`) ist
schwer von Glück zu unterscheiden, weil dieselbe Antwort im `healthy`-Szenario
falsch war.

**Was gut funktioniert: die Form.** Parse 9/9 und Gate 9/9 mit dem constrained
Decoder. Die Hypothese aus #53 hält hier — das Gerüst hebt die Parse-Rate
unabhängig vom Modell, und die Frage verschiebt sich von „kann es die Form" zu
„wählt es richtig". Die Antwort auf die zweite Frage ist auf dieser Suite: nein.

Laufzeit: rund 25–30 Minuten für neun Fälle auf einem Pi 5 (Release-Build, vier
Kerne). Die **Inferenzzeit pro Fall ist nicht instrumentiert** — der Harness
misst sie nicht, und eine Uhr in den Eval-Pfad zu legen ist eine Änderung, die
zu Phase 10 gehört, wo Latenz eine der Zielmetriken ist.

### Remote-Modell

Nicht konfiguriert, also nicht gelaufen. Ein nicht verfügbares Remote-Modell
wird als Fehler-Record ausgewiesen und **nicht** als bestanden gewertet.

### Was die Halluzinationsmetrik erst falsch gemessen hat

Die erste Fassung meldete für denselben Lauf **8 von 9 Halluzinationen**. Das
war ein Artefakt: sie las nur das zweite Token des Reasons und verglich es mit
den Prozess-IDs des Snapshots. Gemma schreibt aber
`memory_pressure [critical_app, batch_job]` — das zweite Token ist
`[critical_app,`, und die genannten Prozesse sind **beide** im Zustand
vorhanden.

Aufgefallen ist es erst, weil die Metrik den Reason im Klartext mit ausgibt.
Eine Zahl, die niemand nachprüfen kann, ist eine Zahl, der niemand trauen
sollte.

Korrigiert wird jetzt jedes Token nach der Kategorie geprüft, befreit von
`[](),`, und nur Tokens mit Unterstrich gelten als ID-Behauptung — die
Namenskonvention der Profile (`critical_app`, `batch_job`), die Prosa nicht
erfüllt. Beide echten Gemma-Antworten sind als Regressionsfälle in
`diagnosis_negative.spg` eingefroren, ebenso `healthy))`, das die
Kategorie-Extraktion an Satzzeichen scheitern ließ.

## Wo einfache Regeln offensichtlich genügen

Ehrlich vorweg, bevor Phase 5 es misst: mindestens die Fälle 1, 3 und 6 sind
mit drei Schwellwerten zu lösen — „CPU hoch **und** der Verursacher ist ein
Batch-Prozess", „Speicher hoch **und** ein RSS dominiert", „alles unter
Schwelle". Für diese Fälle ist ein Modell teurer, langsamer und weniger
vorhersagbar als vier Zeilen C.

Nach dem Gemma-Lauf ist die Frage schärfer gestellt: eine Regel, die nur
`temperature-mc > 75000` prüft, löst die Fälle 4 und 7 sofort — Gemma löst
keinen davon. Die Regel-Baseline aus Phase 5 tritt also nicht gegen ein starkes
Modell an.

Interessant sind die Fälle, in denen eine Regel eine Kollision hat:

- **Fall 4 und 7**: Temperatur hoch — bei niedriger Last ist es Kühlung, bei
  hoher Last könnte es beides sein. Eine Regel muss sich für eine Priorität
  entscheiden; ob die vom Modell gewählte besser ist, misst Phase 5.
- **Fall 2 gegen 1**: identische Last, unterschiedliche Ursache. Eine Regel
  schafft das, sobald sie die Rolle liest — was sie tun sollte. Wenn das Modell
  hier nicht deutlich besser ist, ist das ein Befund gegen die Kernhypothese.
- **Fall 5 und 9**: Enthaltung. Regeln können abstinent sein; ob ein Modell das
  auch ist, wenn es nichts weiß, ist die eigentliche Frage.

## Reproduzieren

```
geistshell eval examples/eval/machine/diagnosis.spg
geistshell eval examples/eval/machine/diagnosis_negative.spg
```

Modelllauf (braucht ein GGUF, `ln -s ~/models models`):

```
geistshell eval examples/eval/machine/diagnosis_model.spg --constrained --samples 5
```

Bei nichtdeterministischen Modellen zählt k/N über alle Samples; es gibt kein
Best-of-N.

## Was Phase 4 nicht tut

Keine schreibende Aktion, keine Regel-Baseline (#65), kein Modelltraining. Ohne
die Baseline aus Phase 5 ist jede Modellzahl hier eine Zahl ohne Vergleich.
