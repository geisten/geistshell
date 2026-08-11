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

## Der Watchdog

Jeder schreibbare Kanal **muss** einen sicheren Wert deklarieren
(`heater:1:0:100:w:0`). Ohne ihn träfe die Entscheidung bei Kontaktverlust der
Wert, den die Maschine zuletzt gehört hat — der Zustand mit der geringsten
Wahrscheinlichkeit, sicher zu sein.

Der Watchdog wird **vor** dem Schreiben geprüft, nicht danach. Eine Maschine,
die nicht mehr antwortet, ist eine Maschine mit unbekanntem Zustand; der
einzige Befehl, der dann noch versucht gehört, ist der sichere Zustand — nicht
der nächste Sollwert, den ein Modell gewählt hat.

`spg_device_safe_state` versucht **alle** Kanäle und meldet den ersten Fehler.
Eine Anlage, die auf halbem Weg in den sicheren Zustand stehenbleibt, ist
schlimmer als beide Endzustände.

**Die Einheit ist die des Aufrufers.** Es gibt keine richtige feste Einheit:
das CLI-Kommando übergibt Nanosekunden, der Agenten-Loop rechnet in Schritten
(`step + 1`), und genau das macht seinen Replay deterministisch. Eine
Millisekunden-Frist gegen einen Schrittzähler zu prüfen wäre eine Zahl, die wie
Zeit aussieht und keine ist. Deshalb heißt das Flag `--device-watchdog-steps`.

## Die Aktion

`SPG_ACTION_DEVICE_WRITE` — die erste **nicht umkehrbare** Aktion im
geschlossenen Aktionsraum. Der Kommentar am Enum verlangte dafür „eine stärkere
Geschichte als 'die Policy hat ja gesagt'". Sie besteht aus drei Teilen:

1. Die Kanaltabelle begrenzt sie. Ein Wert außerhalb wird abgelehnt, bevor
   überhaupt ein Socket geöffnet wird — das Modell kann keine Zahl verlangen,
   die der Betreiber nicht freigegeben hat.
2. Jeder schreibbare Kanal deklariert seinen sicheren Wert, und der Watchdog
   fährt ihn an.
3. `device` ist eine **eigene** Capability, getrennt von `machine_process`. Wer
   einen Agenten wollte, der einen Amoklauf-Prozess pausieren darf, bekommt
   nicht stillschweigend einen, der eine Heizung aufdreht.

Der beschränkte Decoder brauchte dafür `SPG_SCAFFOLD_NUMBER`: bis hierher waren
**alle** Zahlen im Scaffold Literale (`cost`, `confidence_bp` sind fest), der
Decoder füllte nur Strings. Ein Sollwert ist die erste Zahl, die das Modell
tatsächlich wählt. Ohne diese Maske wäre die Aktion erneut eine gewesen, die
niemand erzeugen kann.

Journalisiert wird **jeder** Ausgang, auch die Ablehnungen — bei einer nicht
umkehrbaren Aktion zählt der Beleg über das, was *nicht* getan wurde, genauso
viel.

## Was bewusst noch fehlt

Skalierung pro Kanal, 32-Bit- und Float-Register, Coils, und mehrere Maschinen
pro Lauf. Alles davon wartet auf eine zweite echte Maschine — bis dahin wäre es
Struktur ohne Aufrufer, und wie die kostet, steht oben in diesem Dokument.
