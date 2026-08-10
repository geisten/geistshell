# Actions — Phase 6

Der Agent kann eine **reversible** Aktion ausführen, ohne Governance oder
Safety zu umgehen: `pause` und `resume` auf Prozessen, die das Profil explizit
verwaltet.

Ticket: geisten/geistshell#66. Voraussetzungen: [Processes.md](Processes.md),
[Context.md](Context.md). Offene Folgelücke: **#80** — pausierte Prozesse
garantiert wieder freigeben.

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
                (pid 4711) (outcome ok))
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

## Was Phase 6 nicht tut

Kein Closed Loop (#67) — der Agent beobachtet die Wirkung seiner Aktion noch
nicht, weil der Snapshot einmal pro Lauf entsteht. Kein `set_nice`. Und
insbesondere **keine Garantie, dass ein pausierter Prozess wieder freigegeben
wird**, wenn der Run stirbt: das ist #80 und sollte vor #67 kommen.
