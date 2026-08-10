# Context — Phase 3

Der Machine State erreicht den Agent Context. Das Modell kann die Maschine
**beobachten**, ohne dass wir ihm dafür Shell-Zugriff geben.

Ticket: geisten/geistshell#63. Voraussetzungen: [Telemetry.md](Telemetry.md),
[Processes.md](Processes.md). Keine neue Action — das ist Phase 6 (#66).

## Der exakte Block

```
(machine-state (cpu-load-bp 9200) (load-1-cbp 175) (memory-total-bytes 4245815296)
 (memory-used-bytes 1682931712) (swap-used-bytes 542932992) (temperature-mc 47950)
 (cpu-freq-khz 1500000) (throttle none) (process-count 167)
 (process (id "critical_app") (role critical) (cpu-bp 5400) (rss-bytes 4096))
 (process (id "batch_job") (role batch) (cpu-bp 3100) (rss-bytes 8192))
 (processes-dropped 98))
```

Gerendert nach `(budgets ...)`, vor Graph und Memory. Feste Feldreihenfolge,
feste Prozessreihenfolge (die aus `spg_process_select`), nur Ganzzahlen.

### `id` statt Name und PID

Eine Form für alle Prozesse: `id` ist die Profil-ID, wenn der Prozess gemanagt
ist, sonst der Prozessname. Zwei Formen würden ein kleines Modell Genauigkeit
kosten, ohne etwas zu gewinnen.

Die **PID fehlt bewusst**. Das Modell braucht sie nie: eine Phase-6-Action
zielt auf die Profil-ID, und der Executor validiert die Identität selbst
(PID + Startzeit, siehe [Processes.md](Processes.md)). Eine PID im Context wäre
eine Zahl, die das Modell zu benutzen versucht und die zwischen Beobachtung und
Ausführung ungültig werden kann.

Ebenso nicht im Context: Command Line, Environment, Benutzername, Pfade.

### Trunkierung ist sichtbar

`(processes-dropped N)` erscheint, sobald die Auswahl etwas verworfen hat. Eine
gekürzte Liste, die vollständig aussieht, lädt das Modell zu falschen Schlüssen
ein („nur diese drei Prozesse laufen"). Ist die Gesamtzahl unbekannt, steht dort
`unknown` statt einer erfundenen Zahl.

## Standardmäßig abwesend

Ohne `--machine` (bzw. ohne gesetztes `sources.machine`) ist der Context
**byte-identisch** zu vorher. Das ist keine Bequemlichkeit, sondern die
Voraussetzung dafür, dass der Journal-Freeze aus [Baseline.md](Baseline.md)
weiter etwas aussagt: er prüft den Default-Pfad.

Ein CLI-Test fährt denselben Lauf zweimal — mit und ohne Flag — und verlangt,
dass der Block im einen Fall wohlgeformt auftaucht und im anderen gar nicht.

## Auf Hardware verifiziert

Pi 5, Kernel 6.18. Gemessener Block eines echten Laufs mit Profil:

```
(machine-state (cpu-load-bp unknown) (load-1-cbp 33) (memory-total-bytes 4245815296)
 (memory-used-bytes 1683259392) (swap-used-bytes 542523392) (temperature-mc 50700)
 (cpu-freq-khz 2200000) (throttle none) (process-count 167)
 (process (id "critical_app") (role critical) (cpu-bp unknown) (rss-bytes 849444864))
 (process (id "batch_job") (role batch) (cpu-bp unknown) (rss-bytes 360660992))
 (process (id "dockerd") (role unknown) (cpu-bp unknown) (rss-bytes 31211520))
 ...)
```

Gemanagte Prozesse zuerst, mit ihrer Rolle und ihrer Profil-ID; danach nach
Speicher absteigend. Blockgröße rund 2,7 KB.

## Kontextkosten

Gemessen mit dem Renderer:

| Inhalt | Bytes |
|---|---|
| Nur Systemfelder, keine Prozesse | 224 |
| + 8 Prozesse | 872 |
| + 16 Prozesse | 1496 |
| + 64 Prozesse (Obergrenze) | 5240 |

Am realen Lauf: das Journal eines Ein-Tick-Agentenlaufs wächst von 2340 auf
2573 Bytes (+233), weil auf der Entwicklungsmaschine alle Werte `unknown` sind
und keine Prozesse anfallen.

`SPG_MACHINE_RENDER_CAP` (8192) ist die Obergrenze, aus der ein Aufrufer einen
Stack-Puffer dimensionieren kann; ein Test füllt einen Snapshot mit 64
maximallangen Einträgen und verlangt, dass er hineinpasst.

**Offene Konsequenz für kleine Modelle:** 64 Prozesse sind über 5 KB und
konkurrieren im Fenster mit der Lesson-Injection aus P6 (#3). Für ein Modell mit
kurzem Fenster ist die Prozessgrenze vermutlich deutlich kleiner zu wählen —
welche Felder wirklich nötig sind, misst Phase 11 (#71).

## Determinismus

- Gleicher Snapshot ⇒ byte-identischer Context (Test vergleicht zwei Renderings).
- Die Reihenfolge der OS-Prozessliste ändert das Ergebnis nicht (Permutations-
  test in Phase 2, die Auswahl liefert die Reihenfolge).
- Locale-unabhängig: der Block enthält ausschließlich Ganzzahlen. Ein Test
  rendert unter `de_DE.UTF-8` und vergleicht Byte für Byte mit `C`.
- Prozessnamen werden escaped (`"` und `\` maskiert). Steuerzeichen hat schon
  der Parser in Phase 2 entfernt. Ein Test verlangt für einen Namen wie
  `ev"il\x` balancierte Klammern — sonst könnte ein Prozessname den Block
  vorzeitig schließen und der Rest würde als Anweisung gelesen (Vorgriff auf
  #76, Angriff 12).

## Prozesse ohne Signal fallen raus

Ein Prozess ohne Speicher und ohne messbare CPU trägt keine Information. Auf
einem Pi 5 sind das rund 100 Kernel-Threads, die im ersten Hardwaretest **58 von
64 Kontextplätzen** mit `(rss-bytes 0)` füllten — rund 4 KB, in denen kein Satz
etwas aussagt. Sie werden vor der Auswahl verworfen.

Gemanagte Prozesse sind ausgenommen: ein pausierter Batch-Job, der gerade nichts
kostet, ist genau das, was das Modell weiterhin sehen muss.

Das Profil wird **während** der Enumeration angewendet, nicht danach. Sonst
liefe die Regel „gemanagte Prozesse zuerst" ins Leere — die Auswahl sortiert
nach einer Rolle, die zu diesem Zeitpunkt noch niemand gesetzt hätte. Auch das
zeigte erst der Pi: ohne diesen Schritt trug jeder Prozess im Context
`role unknown`.

`--process-profile <datei>` lädt das Profil aus Phase 2 und impliziert
`--machine`.

## Ein Sample pro Lauf, nicht pro Tick

`--machine` sampelt einmal beim Start. Das genügt für diese Phase: sie
beweist, dass der State im Context ankommt. Ein neuer Snapshot pro Tick wird
erst sinnvoll, wenn es eine Action gibt, deren Wirkung beobachtet werden muss —
das ist Phase 7 (#67), und sie ändert dafür genau eine Stelle.

Auch hier liest nichts eine Uhr: der Zeitstempel kommt aus demselben injizierten
Zähler wie beim Journal.

**Konsequenz, die man kennen muss:** mit einem einzigen Sample ist `cpu-bp` für
jeden Prozess `unknown` — CPU ist ein Delta. Das Modell sieht in dieser Phase
also Speicher, Temperatur und Rollen, aber keine Prozess-CPU. Ab Phase 7, die
pro Tick neu beobachtet, ist der Wert da.

## Abweichung vom Ticket

Das Ticket verlangte einen erweiterten Eval Case als Nachweis. Stattdessen gibt
es `test/test_cli_machine.sh`: derselbe Lauf zweimal, einmal mit und einmal ohne
`--machine`, mit Prüfung des tatsächlich journalten Model-Inputs auf
Wohlgeformtheit und Pflichtfelder. Das prüft dieselbe Eigenschaft näher an der
Realität — der Nachweis führt über das Journal, nicht über eine Testfixture.

## Was Phase 3 nicht tut

Keine Action, kein History-Fenster (#79), keine Diagnose (#64). Kein
Modellverhalten wird hier gemessen.
