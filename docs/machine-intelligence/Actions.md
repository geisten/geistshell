# Actions — Phasen 6 und 6b

Der Agent kann eine **reversible** Aktion ausführen, ohne Governance oder
Safety zu umgehen: `pause` und `resume` auf Prozessen, die das Profil explizit
verwaltet.

Tickets: geisten/geistshell#66 (die Aktion) und #80 (die Garantie, dass nichts
pausiert bleibt). Voraussetzungen: [Processes.md](Processes.md),
[Context.md](Context.md).

## Kein `local_shell`

```
(recommend (kind machine_pause_process)
           (capability "machine.process.pause")
           (target "batch_job")
           (cost 1) (uses_network false) (confidence_bp 9000)
           (reason "..."))
```

Die Grammatik **verbietet** ein `command`-Feld für Machine Actions. Das ist der
Punkt eines geschlossenen Action Space: es gibt kein Feld, durch das ein
Shell-String einen Executor erreichen könnte. Ein Test prüft genau das —
dieselbe Empfehlung mit `command` wird vom Parser abgelehnt, nicht erst vom
Gate.

`target` ist eine **Profil-ID**, nie eine PID. Das Modell sieht keine PIDs
([Context.md](Context.md)) und kann folglich keine nennen.

## Zwei Schichten, zwei Fragen

**Der Gate entscheidet, OB.** Rolle, Berechtigung, und ob das Ziel überhaupt
beobachtet wurde — alles in `spg_policy_gate_step`, nicht im Executor. Eine
Sicherheitseigenschaft, die erst hinter dem Gate durchgesetzt wird, ist eine,
von der das Journal nicht zeigen kann, dass sie geprüft wurde.

**Der Executor entscheidet, OB ES NOCH DERSELBE IST.** Diese Frage kann der
Gate nicht beantworten: zwischen Entscheidung und Syscall kann eine PID
wiederverwendet werden. Deshalb liest der Executor `/proc/<pid>/stat` unmittelbar
vor `kill()` erneut und vergleicht PID **und** Startzeit.

| Situation | Ergebnis | Wo entschieden |
|---|---|---|
| gemanagter Batch-Prozess, `may_pause true` | ALLOW | Gate |
| `role critical` bzw. `may_pause false` | `DENY_PROCESS_PROTECTED` | Gate |
| Prozess nicht im Profil | `DENY_UNMANAGED_PROCESS` | Gate |
| kein Profil konfiguriert | `DENY_UNMANAGED_PROCESS` | Gate |
| Ziel nicht im aktuellen Snapshot | `DENY_PROCESS_IDENTITY` | Gate |
| Capability fehlt oder falsch | `DENY_UNKNOWN_CAPABILITY` | Gate |
| Budget erschöpft | `DENY_*_BUDGET` | Gate |
| PID gehört inzwischen jemand anderem | `identity_changed`, **kein Signal** | Executor |
| Prozess ist weg (`ESRCH`) | `gone` | Executor |
| kein Recht (`EPERM`) | `forbidden` | Executor |
| PID 1, eigene PID, Elternprozess | `refused` | Executor |
| kein `/proc` (macOS, BSD) | `unsupported`, **kein Signal** | Executor |

**Schutz geht vor Budget.** Ein geschützter Prozess wird abgelehnt, *bevor*
Capability und Budget geprüft werden. Sonst würde ein Ablehnungsgrund im
Journal stehen, der nicht der eigentliche ist — „Budget erschöpft" statt „das
darfst du nicht".

## Resume braucht keine Erlaubnis

`may_pause` gilt für das Pausieren. Resume ist für jeden gemanagten Prozess
erlaubt, weil es nur den Zustand wiederherstellen kann, den die Maschine vor
unserem Eingriff hatte. Es zu verweigern hieße, einen von uns gestoppten
Prozess gestoppt zu lassen.

Restrisiko, benannt: ein `SIGCONT` an einen Prozess, den ein Operator
absichtlich gestoppt hat, würde diese Absicht überschreiben. Nur gemanagte
Prozesse sind betroffen, und die hat der Operator selbst deklariert.

## Nur Linux signalisiert

Der Executor sendet ausschließlich auf Linux Signale — nicht aus
Portabilitätsgründen, sondern weil die Identitätsprüfung `/proc` liest. Ein
Executor, der nicht verifizieren kann, was er gleich stoppt, darf nichts
stoppen. Jede andere Plattform meldet `unsupported`.

Kein `system()`, kein `popen()`, keine Shell. `kill(pid, SIGSTOP)` und
`kill(pid, SIGCONT)`, sonst nichts.

## Alles wird journaled

Jede Entscheidung — ALLOW wie DENY — und jeder Executor-Ausgang landet im
Journal:

```
(machine_action (kind machine_pause_process) (target "batch_job")
                (pid 4711) (identity 987654) (outcome ok))
```

Auch ein `refused` steht dort. Eine Ablehnung ohne Spur ist eine Ablehnung, die
niemand prüfen kann. Replay reproduziert den Verlauf.

## Auf Hardware verifiziert

Pi 5, Kernel 6.18. `test/machine_action_probe.c` forkt ein Kind und treibt den
Executor dagegen:

- `pause` → der Prozesszustand in `/proc` wird `T`
- `resume` → er läuft wieder
- Snapshot mit veränderter Startzeit → `identity_changed`, und der Prozess
  bleibt **unangetastet**

Der letzte Punkt ist der wichtigste Test dieser Phase. Regressiert er,
signalisiert der Agent fremde Prozesse.

`test/test_cli_machine_run.sh` fährt dasselbe über die CLI: pausiert ein echtes
`sleep`, prüft den Zustand, und verlangt für einen `critical`-Prozess
`SPG_POLICY_DENY_PROCESS_PROTECTED` im Journal — „denied" allein würde nicht
zwischen Schutz und Budgetende unterscheiden.

Auf macOS läuft derselbe Test gegen die Verweigerung: ohne Snapshot kein
beobachteter Prozess, also `DENY_PROCESS_IDENTITY` statt eines stillen No-ops.

## Konfiguration

`examples/machine-policy.spg` aktiviert `machine.process.pause` und
`machine.process.resume` — und **kein** `local_shell`. Das ist Absicht: ein
Modell, dem eine Pause verweigert wurde, soll nicht auf `kill` ausweichen
können. Der Bypass-Test in Phase 7 (#67) hängt daran.

```
geistshell agent --config examples/machine-run.spg \
  --machine --process-profile <profil.spg> --allow-exec
```

Ohne `--process-profile` ist nichts gemanagt und jede Machine Action wird
abgelehnt. Ohne `--allow-exec` erreicht sie den Executor, der `unsupported`
meldet, statt zu signalisieren.

## Nichts bleibt pausiert (Phase 6b, #80)

Eine Pause ist nur reversibel, solange sich jemand an sie erinnert. Ohne das
Folgende hinterlässt ein Lauf, der nach `SIGSTOP` stirbt, einen dauerhaft
gestoppten Prozess — der einzige Weg, auf dem diese Runtime bleibenden Schaden
anrichtet, ohne dass eine Policy verletzt wurde.

### Erste Linie: das Ledger

Jede erfolgreiche Pause wird mit PID **und** Startzeit in einem Ledger fester
Größe vermerkt. Am Ende **jedes** Laufs — finish, Fehler, max steps, Budget,
Policy-Deny, Interrupt — wird alles darin wieder freigegeben.

**Eine Pause, die nicht vermerkt werden kann, findet nicht statt.** Kein Ledger
oder ein volles Ledger ⇒ `refused`, bevor das Signal gesendet wird. Die
Alternative wäre ein gestoppter Prozess, dem niemand ein Resume schuldet.

`SIGINT`/`SIGTERM` setzen nur ein `volatile sig_atomic_t`-Flag. Der Handler
gibt selbst nichts frei — das hieße, aus einem Signal-Handler zu journalen. Die
Freigabe macht der normale Aufräumpfad, und `SA_RESTART` ist bewusst **nicht**
gesetzt, damit ein blockierender Read mit `EINTR` zurückkommt, statt zu warten,
während ein Prozess gestoppt bleibt.

### Zweite Linie: Recovery aus dem Journal

`SIGKILL` und Stromausfall sind nicht abfangbar; das Ledger stirbt mit dem
Prozess. Übrig bleibt das Journal. Beim nächsten Start paart
`spg_machine_recover_journal()` Pausen mit Resumes und gibt frei, was offen
blieb — **bevor** der neue Lauf beobachtet, damit der Snapshot nicht einen
Zustand zeigt, den wir selbst hinterlassen haben.

Deshalb steht die Startzeit im Journal-Payload: eine PID allein ist keine
Identität, und Recovery muss einen wiederverwendeten PID überspringen statt ihn
zu wecken.

Recovery ist **wiederholbar**, nicht idempotent im engeren Sinn: ein `SIGCONT`
auf einen laufenden Prozess ist harmlos. Genau das macht es sicher, sie bei
jedem Start bedingungslos auszuführen.

### Verbleibende Lücke, benannt

Zwischen `SIGKILL` und dem nächsten Start bleibt der Prozess gestoppt. Dagegen
hilft nur ein externer Watchdog, und der ist bewusst nicht gebaut — er wäre ein
zweiter Lebenszyklus mit eigenen Fehlermodi für ein Zeitfenster, das ein
Neustart schließt.

Ebenfalls offen: zwei parallele Läufe teilen sich das Journal nicht. Recovery
eines Laufs sieht die Pausen des anderen und könnte sie freigeben. Für einen
Agenten pro Maschine ist das kein Problem; für mehrere wäre es eines.

### Auf Hardware verifiziert

Pi 5. `machine_action_probe` pausiert ein Kind mit aktivem Journal, lässt das
Ledger fallen (die simulierte Katastrophe), ruft Recovery — und das Kind läuft
wieder. Der Absturz wird durch Weglassen der Freigabe simuliert, nicht durch
Töten des Agenten mitten im Signal: das wäre ein Race, und ein Race im Test ist
ein Flake.

`test_cli_machine_recovery.sh` prüft die erste Linie über die CLI: nach einem
Lauf, der pausiert hat, meldet er `released=1` und das Kind läuft.

## Was Phase 6 nicht tut

Kein Closed Loop (#67) — der Agent beobachtet die Wirkung seiner Aktion noch
nicht, weil der Snapshot einmal pro Lauf entsteht. Kein `set_nice`.
