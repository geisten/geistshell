# Workloads und Experimente — Phase 8

Ein realer, wiederholbarer Versuchsstand statt Fixtures. Zum ersten Mal misst
sich der Agent an einer Maschine, die tatsächlich beschäftigt ist.

Ticket: geisten/geistshell#68. Voraussetzung: [Closed-Loop.md](Closed-Loop.md).

## Ein Programm, fünf Modi

`examples/machine/workloads/workload.c` — nicht fünf Programme. Die Modi
unterscheiden sich darin, **was** sie verbrauchen, nicht darin, wie sie
begrenzt, gestartet und gestoppt werden, und dieser gemeinsame Teil ist der,
der stimmen muss.

```
workload --mode cpu|memory|mixed|batch|critical [--seconds N] [--mb N]
```

Dreifach begrenzt: Wanduhr-Deadline, harte Obergrenze für Allokation
(`MAX_MB`, `MAX_SECONDS` als Decken, nicht als Defaults — ein Tippfehler im
Argument darf die Maschine nicht umbringen), und ein Signal-Handler für ein
vorzeitiges Ende. Kein `fork`, kein Socket, keine Rechte nötig, und der
Speicher wird freigegeben.

`memory` fasst jede Seite an: ein nicht berührtes Mapping taucht nicht im RSS
auf, und das Szenario wäre eine Lüge. Schlägt `malloc` fehl, hört es auf statt
es erneut zu versuchen — Ziel ist, Speicher zu belegen, nicht den OOM-Killer zu
wecken.

`batch` und `critical` sind derselbe CPU-Modus bei unterschiedlichem
Nice-Level. **Was sie bedeuten, entscheidet das Profil** — ein Workload
erklärt sich nicht selbst für wichtig.

## Der Runner

`examples/machine/run_experiment.sh` startet die Last, lässt den Agenten
beobachten und handeln, und schreibt einen JSONL-Record.

Das Wichtigste daran ist das Aufräumen. Jeder Ausgang — Erfolg, Fehler,
Abbruch — geht durch denselben `trap`. Pausierte Workloads bekommen zuerst
`SIGCONT`: `SIGTERM` erreicht einen gestoppten Prozess nicht, und ein
Experiment, das eine gestoppte Last hinterlässt, hat die Maschine beschädigt,
die es messen wollte.

### Result Schema

`run_id`, `scenario`, `model`, `workload_observed`, `initial_state`,
`final_state`, `observations`, `actions`, `policy_denials`, `steps`,
`goal_satisfied`, `critical_cpu_bp_final`, `batch_cpu_bp_final`,
`agent_status`, `elapsed_ms`, `energy_mj`.

`energy_mj` ist **immer `null`**: hier misst kein Sensor Energie, und eine
erfundene Zahl wäre schlimmer als die eingestandene Lücke. Machine B (#75) kann
sie füllen.

`run_id` wird aus dem Inhalt abgeleitet, nicht hochgezählt — zwei Records
können sich so nicht still überschreiben.

## Was auf dem Pi gemessen wurde

Pi 5, Kernel 6.18, drei Szenarien:

| Szenario | Last erkannt | Beobachtungen | Aktionen | Ziel erreicht | critical CPU | batch CPU |
|---|---|---|---|---|---|---|
| `batch_pressure` | ja | 2 | 2 | **ja** | 24,95 % | 0 % |
| `mixed` | ja | 2 | 2 | **ja** | 25,04 % | 0 % |
| `idle` | nein (gewollt) | 1 | 0 | nein | – | – |

Der Agent pausiert die Batch-Last, der kritische Prozess läuft weiter, und die
Freigabe am Ende ist die zweite Aktion. Das ist der vollständige Zyklus aus
Phase 7 an echten Prozessen.

## Vier Fehler, die erst der echte Versuchsstand zeigte

**1. Der Loop war im echten Agenten offen.** Phase 7 verdrahtete
`refresh_machine` nur im Eval-Harness. Der `agent`-Befehl entschied jeden Tick
auf dem Snapshot vom Start — die Schleife war in den Tests geschlossen und in
der Realität nicht. Zwei identische Zustände im Record haben es verraten.

**2. Ohne Setzzeit misst man die Vergangenheit.** Die erste Messung meldete
100 % CPU **nach** erfolgreicher Pause. CPU-Auslastung ist ein Delta: ein
Sample Mikrosekunden nach `SIGSTOP` beschreibt noch die Last davor. Der Loop
kennt jetzt `machine_settle_ms` (`--machine-settle-ms`, Default 0), und der
Runner wartet 1,5 s. Das ist ein Beobachtungsparameter, kein verstecktes
Verhalten — deshalb setzt ihn der Aufrufer.

**3. Beide Workloads hießen gleich.** Das Profil matcht auf `comm`, also den
Programmnamen. Zwei Rollen aus einer Binärdatei sind ununterscheidbar: der
erste echte Lauf markierte den **kritischen** Prozess als `batch_job` und
pausierte ihn. Der Runner legt jetzt pro Rolle eine Kopie unter eigenem Namen
an.

**4. Das Ziel war an der falschen Größe gemessen.** Meine erste Zielprüfung
verglich die Systemlast vorher/nachher — aber das erste Sample eines Laufs hat
keine Vorgängerzähler, die Systemlast ist dort `unknown`, und damit wäre jedes
Experiment als gescheitert gewertet worden. Gemessen wird jetzt, was messbar
ist: `batch_job` bei 0 % **und** `critical_app` über 0 % in der letzten
Beobachtung.

Dazu zwei Portabilitätsfehler, die nur der Pi fangen konnte: `nice()` ist XSI
und unter reinem `_POSIX_C_SOURCE` auf glibc nicht deklariert (jetzt
`setpriority`), und `nanosleep` braucht dieselbe Feature-Test-Macro. Beide
kompilierten auf macOS anstandslos.

## CI-Modus

`--ci` fährt dieselben Codepfade in klein (3 s, 16 MB). Es ist ein kleineres
Experiment, kein anderes.

`test/test_cli_experiment.sh` prüft, was ein Smoke-Test prüfen kann: der
Workload endet an seiner eigenen Deadline, Argumente können die Decken nicht
überschreiten, jeder Lauf schreibt einen vollständigen Record, zwei Läufe haben
verschiedene `run_id`, ein Runner ohne Workload-Binary **scheitert laut** statt
eine leere Maschine als Befund zu melden, und nach einem `SIGINT` mitten im
Experiment läuft kein Workload mehr.

## Grenzen

- **Der Agent ist scripted.** Der Runner kennt `--model geist`, aber die
  Messungen oben fuhren die Fake-Modell-Skripte: hier wird der Versuchsstand
  geprüft, nicht das Modell. Modelle gegeneinander zu messen ist Phase 10.
- **`goal_satisfied` ist eine Heuristik dieses Szenarios**, kein allgemeines
  Zielmodell. Explizite Ziele und ihre objektive Prüfung sind Phase 9 (#69).
- **Eine Beobachtung nach der Aktion**, nicht mehrere. Für einen Zyklus reicht
  das; eine Zeitreihe verlangt Phase 9 oder 10.
