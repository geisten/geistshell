# Model Tournament — Phase 10

Dieselben Szenarien, verschiedene Modelle, eine Tabelle.

Ticket: geisten/geistshell#70. Voraussetzungen:
[Diagnosis-Benchmark.md](Diagnosis-Benchmark.md), [Goals.md](Goals.md).

## Kein zweiter Harness

Jeder Teilnehmer läuft durch den **vorhandenen** Modell-Adapter und den
**vorhandenen** Eval-Harness. Ein eigener Turnier-Pfad würde zwei Harnesses
vergleichen statt zwei Modelle.

`examples/machine/run_tournament.sh` erzeugt pro Modell eine Run-Config (der
Modellpfad steht dort, nicht in der Umgebung), fährt die Suite und schreibt
eine Zeile nach `machine-benchmark.jsonl`. Am Ende entsteht
`machine-benchmark.md`.

## Latenz, endlich gemessen

Phase 4 ließ die Inferenzzeit offen. Der Harness misst jetzt Wanduhr pro Fall
und Peak-RSS für den Prozess — **hinter `--timing`, standardmäßig aus.**

Der Grund für den Schalter ist kein Geschmack: zwei identische Eval-Läufe
müssen bitgleiche Reports liefern. Ein Wanduhr-Wert im Standard-Output bricht
jeden Diff und den Fixture-Test, der genau darauf beruht. Ein Report ist zum
Vergleichen da; eine Messung gehört hinter einen Schalter. Der bestehende Test
hat mich darauf gestoßen, nachdem ich die Felder zuerst unbedingt ausgegeben
hatte.

Die Runtime selbst liest weiterhin keine Uhr — das ist es, was Replay
byte-identisch hält. Gemessen wird im Harness, und nichts davon erreicht
Journal oder Context.

## Was in der Tabelle steht

| Spalte | Woher | Grenze |
|---|---|---|
| Size | `stat` der GGUF-Datei | – |
| Known / Held-out | getrennt, nie gemittelt | – |
| Unsafe | Aktionsvorschlag in einer reinen Diagnose-Suite | – |
| Reject | Anteil nicht geparster Empfehlungen | – |
| Deny | geparst, aber vom Gate abgelehnt | eigene Spalte, nicht mit Reject vermischt |
| Steps / p95 | pro Fall | p95 über neun Fälle ist eine Beschreibung, keine Statistik |
| Latency | ganze Suite auf diesem Host | **nicht** pro Inferenz — dafür bräuchte es Instrumentierung im Adapter |
| Peak RSS | Harness-Prozess | enthält das Modell, aber auch den Harness |

Reject und Deny bleiben getrennt: eine Empfehlung, die nicht parst, und eine,
die geparst und abgelehnt wurde, sind verschiedene Fehler. Sie zu addieren
verwischt genau den Unterschied, den die Leiter aus #53 sichtbar macht.

## Wer nicht antritt, steht trotzdem in der Tabelle

Ein Modell, das nicht geladen werden konnte, bekommt eine Zeile mit
`not run (status N)` — nicht das Weglassen. Eine Tabelle, in der ein Teilnehmer
fehlt, lädt den Leser ein anzunehmen, es sei schon in Ordnung gewesen.

Ein nicht konfiguriertes Remote-Modell ist genau dieser Fall und wird **nie**
als bestanden gewertet.

## Kein Best-of-N

Bei `--samples N > 1` berichtet der Harness k/N pro Fall. Es gibt keinen Pfad,
der den besten Lauf auswählt — nicht weil es niemand tut, sondern weil es ihn
nicht gibt. Die answer-free Selektion aus #55 ist die Alternative, wo eine
Auswahl gebraucht wird.

## Gemessen auf dem Pi 5

| Model | Size | Known | Held-out | Unsafe | Reject | Steps | Latency | Peak RSS |
|---|---|---|---|---|---|---|---|---|
| fake (scripted) | – | 6/6 | 3/3 | 0 | 0 | 1,0 | 1 ms | 3,7 MB |
| Gemma4-E2B Q4_K_M | 2963 MB | **2/6** | **1/3** | 0 | 0 | 1,0 | 254 852 ms | 1,82 GB |
| remote | – | nicht gelaufen (kein `GEISTSHELL_API_URL`) | | | | | | |

Rund 28 Sekunden pro Fall auf vier Kernen. Peak-RSS liegt unter der Dateigröße,
weil die Gewichte gemappt und nicht kopiert werden.

Das Ergebnis deckt sich exakt mit Phase 4 (2/6 known, 1/3 held-out) — dasselbe
Modell, dieselbe Suite, dieselbe Zahl. Zwei unabhängige Läufe über
verschiedene Codepfade, die dasselbe sagen, sind mehr wert als ein Lauf mit
einer schöneren Zahl.

Die Form sitzt, die Entscheidung nicht: Parse und Gate bleiben vollständig, das
Modell wählt nur die falsche Ursache.

## Grenzen

- **Latenz ist Suite-Latenz.** Pro-Token- oder Pro-Inferenz-Zeiten bräuchten
  eine Uhr im Adapter. Das ist eine Änderung an der Inferenzschicht und keine
  am Harness — sie gehört dorthin, wo jemand sie braucht.
- **Peak RSS ist Prozess-RSS.** Modell und Harness sind darin nicht getrennt.
  Für einen Größenvergleich zwischen Modellen ist die Differenz zum
  Fake-Adapter der ehrlichere Wert.
- **CPU-Auslastung während der Inferenz wird nicht gemessen.** Das Ticket
  nennt sie; sie bräuchte einen Sampler neben dem laufenden Modell, und der
  wäre selbst Last. Nicht gemessen ist besser als falsch gemessen.
- **Energie: `null`.** Wie überall, bis #75 etwas hat, das sie misst.
