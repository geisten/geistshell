# Processes — Phase 2

Prozess-Telemetrie mit semantischen Rollen. Der Agent sieht nicht nur „CPU
90 %", sondern **wer** verbraucht und **was dieser Prozess bedeutet**.

Ticket: geisten/geistshell#62. Voraussetzung: [Telemetry.md](Telemetry.md).
Noch keine Aktion — Pausieren und Stoppen ist Phase 6 (#66).

## Was nicht erfasst wird

Keine Command Line, keine Environment-Variablen, kein Benutzername. Diese
Felder tragen regelmäßig Secrets (`--password=`, `AWS_SECRET_...`) und wären ab
Phase 3 Prompt-Injection-Fläche im Model-Context. Was nicht erfasst wird, kann
nicht leaken.

Erfasst werden: PID, Start-Identität, Name, Zustand, Nice, CPU-Zeit, RSS.

## Identität statt PID

`start_identity` ist die Startzeit des Prozesses in Ticks seit Boot
(`/proc/<pid>/stat` Feld 22). Zusammen mit der PID ist das die
Prozessidentität.

Der Grund ist Phase 6: PIDs werden wiederverwendet. Ein Agent, der in Tick 1
„PID 4711 verbraucht 90 % CPU" beobachtet und in Tick 2 `SIGSTOP` an 4711
schickt, trifft möglicherweise einen völlig anderen Prozess. Deshalb:

- `spg_process_find()` sucht **nur** über PID **und** Start-Identität.
- `spg_process_utilisation_bp()` liefert `unknown`, wenn die Identität nicht
  übereinstimmt — lieber kein Wert als ein erfundener.
- Ein `/proc/<pid>/stat`, das vor Feld 22 abbricht, wird komplett verworfen:
  ein Sample ohne Identität darf Phase 6 nie erreichen.

## Der Name ist kein Weg

`comm` schreibt der Kernel **unescaped in Klammern**, und er darf Leerzeichen
und `)` enthalten. `"weird (name) here"` ist ein legaler Prozessname. Der
Parser verankert sich deshalb an der **letzten** `)` der Zeile, nicht an der
ersten — der klassische Fehler in selbstgebauten `/proc`-Parsern.

Zusätzlich kürzt der Kernel `comm` auf 15 Zeichen (`TASK_COMM_LEN - 1`). Das
Profil-Matching berücksichtigt das:

| Profil `match` | `/proc` meldet | Trifft? |
|---|---|---|
| `batch-worker` | `batch-worker` | ja (exakt) |
| `very-long-worker-name` | `very-long-worke` | ja (Kernel-Kürzung) |
| `batch-worker` | `batch-worker-2` | **nein** (kein Substring-Matching) |
| `very-long-worker-name` | `very-long` | **nein** (nur zufälliger Präfix) |

Kein Substring-, kein Glob-, kein Regex-Matching. Wenn zwei Einträge denselben
Namen treffen, gewinnt der **erste** in der Datei — deterministisch und
dokumentiert.

Steuerzeichen im Namen werden beim Parsen durch `?` ersetzt. Ein Prozessname
mit `\n` oder `"` würde sonst ab Phase 3 die S-Expression zerlegen, in der der
Context besteht.

## Process Profile

```
(process-profile
  (process "critical_app"
    (match "critical-worker")
    (role critical)
    (may_pause false)
    (may_stop false))
  (process "batch_job"
    (match "batch-worker")
    (role batch)
    (may_pause true)
    (may_stop true)))
```

Geparst mit dem vorhandenen S-Expression-Reader, in derselben Form wie
`policy_config.c` — ein DSL, nicht zwei.

`role`, `may_pause` und `may_stop` sind **typed Felder** in
`struct spg_process_sample`, kein Prompt-Text. Phase 6 liest sie aus der
Policy-Schicht; das Modell kann sie nicht überreden.

Was der Parser ablehnt, und warum:

| Eingabe | Status | Grund |
|---|---|---|
| `(role criticl)` | `SPG_E_SCHEMA` | Ein Tippfehler darf einen kritischen Prozess nicht stillschweigend zu `unknown` degradieren |
| fehlendes `may_pause` | `SPG_E_SCHEMA` | Berechtigungen sind nie implizit |
| `role critical` + `may_pause true` | `SPG_E_SCHEMA` | Widerspruch, den der Autor nicht gemeint hat — nicht erst in Phase 6 auflösen |
| doppelte `id` | `SPG_E_SCHEMA` | Das Action-Target in Phase 6 wäre mehrdeutig |
| `match` länger als das Feld | `SPG_E_SCHEMA` | Eine still gekürzte Match-String träfe den falschen Prozess |
| leeres Profil / leere Datei | `SPG_OK` | Nichts gemanagt ist eine gültige Konfiguration |

Ein Prozess ohne Profil-Treffer bekommt `role unknown` und **beide
Berechtigungen `false`**. Auf dieser Eigenschaft baut Phase 6 auf: unbekannte
Prozesse sind nie implizit pausierbar.

Die Strings werden in feste Puffer **kopiert**, nicht als Spans in den
Config-Text gehalten (wie es `policy_config.c` tut). Ein Profil überlebt den
Puffer, aus dem es geparst wurde, und wird jeden Tick gegen Prozessnamen
verglichen.

## Auswahl: der Snapshot ist kleiner als die Maschine

Höchstens `SPG_MACHINE_MAX_PROCESSES` (64) Prozesse erreichen den Snapshot.
`spg_process_select()` entscheidet in fester Reihenfolge:

1. Profil-gemanagte Prozesse
2. CPU absteigend
3. RSS absteigend
4. PID aufsteigend

Betrachtet werden **alle** Prozesse, nicht die ersten 64. Der Enumerator hält
einen laufenden Best-of-Puffer (`spg_process_offer`) und ersetzt darin den
schwächsten Eintrag, sobald ein stärkerer auftaucht. Auch das stammt aus dem
Hardwaretest: die erste Fassung sampelte den Präfix, den `/proc` zufällig
zuerst auflistete, und füllte den Snapshot auf einem Pi 5 mit 167 Prozessen mit
Kernel-Threads bei 0 % CPU, während `geist` mit 856 MB RSS nie betrachtet
wurde. Auf der Entwicklungsmaschine mit Fixtures war das unsichtbar.

Die vierte Stufe ist nicht Kosmetik: `/proc` gibt keine Reihenfolge-Garantie,
und wenn die Enumerationsreihenfolge ins Ergebnis durchschlüge, unterschiede
sich der Context zwischen zwei Läufen mit identischem Zustand — Replay wäre
tot. Ein Test permutiert die Eingabe und verlangt bitgleiche Ausgabe.

`unknown` bei CPU oder RSS sortiert **hinten**, nicht als Riesenwert — sonst
verdrängte ein Prozess ohne Messwert die tatsächlich heißen.

Wird etwas verworfen, setzt die Auswahl `processes_truncated`. Ab Phase 3 muss
das im Context sichtbar sein: eine gekürzte Liste, die vollständig aussieht,
lädt das Modell zu falschen Schlüssen ein.

## Grenzfälle mit Test

PID-Reuse zwischen zwei Ticks, Prozess verschwindet zwischen `readdir` und
`open` (`ENOENT` → übersprungen), Name mit Klammern und Leerzeichen, Name mit
Steuerzeichen, Name über Kernel-Länge, `/proc/<pid>/stat` vor Feld 22
abgeschnitten, keine schließende Klammer, nicht-numerische PID, leere Eingabe,
Overflow in den Zähler-Feldern, mehr Prozesse als Kapazität, leere
Prozessliste, unbekannter Prozess, doppelter Profil-Treffer, Multi-Core-Prozess
über 100 % (auf 10000 bp geclamped).

## Auf Hardware verifiziert

Pi 5 Model B (aarch64, Kernel 6.18): `make test` grün, 26 PASS, warnungsfrei,
ASan/UBSan sauber. Der gemessene Snapshot enthält nach der Korrektur die
tatsächlich relevanten Prozesse — nach CPU, dann RSS: der Messprozess selbst
(5,72 %), `geist` (856 MB), `python3` (360 MB), `dockerd`, `ollama`,
`containerd` — bei 167 Prozessen insgesamt und gesetztem
`processes_truncated`.

## Was Phase 2 nicht tut

Keine Aktion, keine Context-Integration (#63), kein History-Fenster (#79). Der
Journal-Freeze aus Phase 0 bleibt unverändert grün — kein Agent-Pfad ruft die
Prozess-Telemetrie auf.
