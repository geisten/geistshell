# Devices — Maschinen lesen und stellen

Der erste Aktuator, der tatsächlich etwas bewegt.

## Warum Modbus TCP und nicht GPIO

Ein Kanal soll auf einer simulierten und einer echten Maschine **denselben
Codepfad** nehmen. Sonst entsteht ein Simulationszweig, der verrottet, während
niemand hinsieht — genau der Fehler, an dem die Lüfterklemmung aus Phase 15
gestorben ist.

Zwei Ebenen bieten diese Eigenschaft:

| Ebene | Simulator | Warum identisch |
|---|---|---|
| Peripherie | `gpio-sim`, `i2c-stub`, `iio_dummy` | Kernel-Module, die dasselbe `/dev/gpiochipN` bzw. denselben I²C-Bus exportieren wie echte Hardware |
| Maschine | OpenPLC, Factory I/O, `heater.py` | Modbus TCP ist das Protokoll echter Industriemaschinen, nicht eine Testschnittstelle |

Modbus gewinnt als Einstieg, weil es über TCP läuft: entwickelbar und
verifizierbar auf jeder Maschine, ohne Hardware und ohne Linux.

## Die Portgrenze

Ein **Kanal** ist das gesamte Vokabular — Name, Bereich, schreibbar ja/nein.
Nicht eine Funktion pro Peripherie: diese Form hat drei Backends je zwei
Fan-Funktionen tragen lassen, für ein Gerät, das kein Executor je angesteuert
hat. Eine neue Maschine ist eine neue Tabelle, kein neuer Code.

```
geistshell device --port 5502 \
    --channel temp:0:-400:9000:r \
    --channel heater:1:0:100:w \
    read temp
```

## Bereich heißt Ablehnung, nicht Klemmung

Ein Wert außerhalb des Bereichs wird **abgelehnt und nicht gesendet**
(`SPG_E_LIMIT`). Klemmen führt einen Beinahe-Treffer eines bereits falschen
Befehls aus — so gehen Maschinen kaputt an Software, die hilfsbereit sein
wollte. Der Aufrufer erfährt, dass sein Wert verworfen wurde.

**Jede Ablehnung wird vor dem Socket entschieden.** Kanal unbekannt,
schreibgeschützt, außerhalb des Bereichs: alles testbar mit `fd == -1`. Eine
Sicherheitsprüfung, die erst greift, wenn eine Maschine angeschlossen ist, wird
an dem Tag zum ersten Mal ausgeführt, an dem sie gebraucht wird.

Drei weitere Eigenschaften mit demselben Motiv:

- **Timeout in beide Richtungen.** Eine Maschine, die die Verbindung annimmt
  und dann schweigt, darf die regelnde Schleife nicht anhalten.
- **Transaktions-ID wird geprüft.** Eine fremde Antwort ist
  `SPG_E_REPLAY_MISMATCH`, keine Warnung: hinter einem Gateway ordnet man sonst
  die Messung einer Maschine einer anderen zu.
- **Echo wird verglichen.** Quittiert das Gerät einen anderen Wert als
  befohlen, ist das ein Fehler — sonst baut die nächste Entscheidung auf einer
  Zahl auf, die die Maschine nie angenommen hat.

## Die simulierte Anlage

`examples/machine/plant/heater.py` — ein beheizter Kessel mit Wärmeträgheit,
Verlust an die Umgebung und **rastender Übertemperaturabschaltung** bei 90 °C.
Nur Standardbibliothek.

Die Abschaltung ist der Punkt. Eine Anlage, die nur warm wird, belohnt jeden
Regler, der die Heizung aufreißt, und misst damit nichts. Diese hier sperrt
sich aus und muss zurückgesetzt werden — sie bestraft genau den Regler, der
handelt, ohne vorher zu schauen.

Zwei Modellfehler, beide beim ersten Lauf gefunden:

1. **Der Trip war unerreichbar.** Bei 100 % lag das Gleichgewicht bei 50 °C,
   die Abschaltung bei 90 °C. Eine Grenze, die keine Eingabe erreichen kann,
   ist Dekoration. Jetzt liegt das Gleichgewicht bei 145 °C, und alles über
   ~56 % rastet irgendwann aus.
2. **Der Simulator hat die Messung gestört.** `--speed` erhöhte ursprünglich
   die Tickrate — bei Faktor 40 waren das 800 Wakeups pro Sekunde, genug, um
   den lastempfindlichen `test_cli_machine_run` in derselben Suite kippen zu
   lassen. Jetzt skaliert `--speed` die Physik pro Tick; die Rate bleibt bei
   20 Hz. Eine Testanlage, die die gemessene Maschine beeinflusst, ist keine
   Fixture.

## Was bewusst noch fehlt

Es gibt **keine Agenten-Aktion** für Geräte. Das ist die Reihenfolge aus der
Lüfter-Lehre: Executor zuerst, Aktion zuletzt. `machine_set_fan` existierte in
Policy, Grammatik-Maske und CLI-Switch, und nichts hat sie je ausgeführt. Eine
Aktion kommt, wenn der Executor nachweislich etwas bewegt — das tut er seit
diesem Commit, also ist der nächste Schritt fällig, aber er ist ein eigener.

Ebenso offen: Skalierung pro Kanal, 32-Bit- und Float-Register, Coils, und ein
Watchdog, der bei ausbleibendem Kontakt in einen sicheren Zustand fährt. Der
Watchdog ist der einzige davon, der vor einer echten Maschine stehen muss.
