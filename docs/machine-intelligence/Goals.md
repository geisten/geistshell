# Ziele und Constraints — Phase 9

Ein Lauf ist nicht erfolgreich, weil das Modell `finish` sagt.

Ticket: geisten/geistshell#69. Voraussetzung: [Closed-Loop.md](Closed-Loop.md).

## Vier Schichten, bewusst getrennt

```
(machine-goal
  (max-temperature-mc 70000)
  (min-critical-service-health-bp 9500)
  (prefer-min-energy true)
  (max-actions 3))
```

| Schicht | Wo | Wer prüft |
|---|---|---|
| **Harte Constraints** | `(machine-goal ...)` | der Harness, gegen den beobachteten Endzustand |
| **Optimierungspräferenz** | `prefer-min-energy` | niemand — sie wird protokolliert, nicht bewertet |
| **Semantische Prozessrestriktionen** | Process Profile (#62) | der Policy Gate |
| **Policy und Safety** | Policy Config | der Policy Gate |

Diese Trennung ist der Punkt. Ein Ziel kann die Schichten 2–4 **nie**
erweitern: Etwas zu verlangen, das die Policy verbietet, macht es nicht
erlaubt, sondern das Ziel unerreichbar. Die Richtung geht nur in eine Seite —
ein Ziel darf weniger verlangen als die Policy zulässt, nie mehr.

`prefer-min-energy` wird bewusst nicht bewertet: hier misst nichts Energie
(#75). Eine Präferenz, die als Kriterium durchginge, ohne dass jemand sie
messen kann, wäre schlimmer als keine.

## Was „erreicht" heißt

`spg_machine_goal_evaluate()` liest den **beobachteten** Endzustand und die
**tatsächlich ausgeführte** Aktionszahl. Das Modell kommt in dieser Rechnung
nicht vor.

| Verdikt | Bedeutung |
|---|---|
| `satisfied` | alle harten Constraints erfüllt |
| `temperature_too_high` | Grenzwert überschritten |
| `critical_unhealthy` | ein kritischer Prozess ist gestoppt oder verschwunden |
| `too_many_actions` | mehr Aktionen ausgeführt als erlaubt |
| `unmeasurable` | ein Constraint existiert, sein Wert war nicht lesbar |
| `no_goal` | kein Ziel konfiguriert |

Das Verdikt trägt die gemessenen Werte mit sich. Ein Harness, der nur
„gescheitert" erfährt, kann eine heiße Maschine nicht von einer verschwendeten
unterscheiden.

### Unmessbar ist kein Erfolg

Ein Constraint, dessen Wert nicht gelesen werden konnte, gilt als **nicht
erfüllt**. Ein defekter Sensor ist kein Beleg dafür, dass ein Grenzwert
eingehalten wurde — er wurde nur nicht widerlegt.

Dasselbe gilt für `min-critical-service-health-bp` auf einer Maschine, in deren
Snapshot kein kritischer Prozess vorkommt: 100 % zu melden hieße, ein Ziel zu
bestehen, dessen Gegenstand nie identifiziert wurde.

### `no_goal` ist nicht `satisfied`

Ein Lauf ohne Ziel hat kein Ziel erreicht. Ihn als bestanden zu zählen würde
jede Zusammenfassung aufblähen, in der er vorkommt.

### Health, erste Fassung

10000 bp, wenn jeder als kritisch markierte Prozess vorhanden und nicht
gestoppt ist; 0, wenn einer gestoppt (`T`) oder ein Zombie (`Z`) ist. Das ist
Verfügbarkeit, nicht Dienstgüte — eine feinere Definition braucht etwas, das
den Dienst selbst misst, und das ist Machine-B-Gebiet (#75).

## Der Nachweis

Drei Szenarien in `examples/eval/machine/closed_loop_goal.spg`. **Alle drei
enden damit, dass das Modell dasselbe `finish` ausgibt.** Was sie unterscheidet,
ist die Maschine, die sie hinterlassen:

| Fall | Termination | Verdikt | Ergebnis |
|---|---|---|---|
| `false_finish_while_hot` | `finished` | `temperature_too_high` | **fail** |
| `goal_met` | `finished` | `satisfied` | pass |
| `acted_when_forbidden` | `finished` | `too_many_actions` | **fail** |

Der erste Fall ist der, für den die Phase existiert: sauber terminiert, korrekt
geparst, vom Gate zugelassen — und trotzdem gescheitert, weil die Maschine bei
82 °C steht. Kann die objektive Prüfung ein `pass` nicht **überstimmen**, ist
sie Dekoration.

Der dritte trennt Policy von Ziel: die Aktion war von der Policy erlaubt, das
**Ziel** verbietet sie. Zwei Schichten, zwei Antworten.

## Aktionen, nicht Schritte

`max-actions` wird gegen `actions_executed` geprüft — was der Lauf **getan**
hat, nicht wie oft das Modell gefragt wurde. Ein Modell, das dreimal
nachdenkt und einmal handelt, hat eine Aktion verbraucht.

`(max-actions 0)` wird **nicht** als Widerspruch abgelehnt. „Prüfen, ohne etwas
anzufassen" ist ein legitimer Lauf; eine Maschine, die ihre Constraints bereits
erfüllt, besteht ihn. Jede Aktion lässt ihn scheitern.

## Im Context

Das Ziel wird **vor** dem Zustand gerendert: erst wofür der Lauf da ist, dann
was er sieht. Ein Modell, das die Constraints nach den Zahlen liest, muss die
Zahlen erneut lesen.

Ein Roundtrip-Test verlangt, dass der gerenderte Block wieder als dasselbe Ziel
gelesen wird — sonst beschreiben Context und Verdikt verschiedene Läufe.

## Grenzen

- **Kein Optimierungsframework.** Constraints sind Schwellwerte, keine
  Zielfunktion. Die erste Fassung braucht das laut Ticket nicht, und ohne
  Energiemessung wäre eine Zielfunktion ohnehin unvollständig.
- **Ein Endzustand, keine Trajektorie.** Ob die Temperatur *zwischendurch* über
  dem Grenzwert lag, sieht diese Prüfung nicht. Für ein Ziel der Form „nie
  überschreiten" bräuchte es das History-Fenster aus #79.
- **`impossible` ist kein eigenes Verdikt.** Ein unerreichbares Ziel endet als
  `temperature_too_high` oder `unmeasurable` — korrekt, aber es sagt nicht
  „das war nie zu schaffen". Das zu unterscheiden braucht eine Vorstellung
  davon, was die Maschine überhaupt könnte, und die hat der Harness nicht.
