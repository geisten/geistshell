# Devices — Maschinen lesen und stellen

Der erste Aktuator, der tatsächlich etwas bewegt — und die Grenze, an der
geistshell aufhört, etwas über Maschinen zu wissen.

> **Stand.** Alles hier ist gebaut: die Kanaltabelle, die Bereichsprüfung,
> der sichere Wert, der Watchdog, `SPG_ACTION_DEVICE_WRITE`, der
> Sensorrückweg (`(device-state …)` im Kontext), der `exec`-Transport und
> die `(device …)`-Konfigurationsform. Modbus TCP ist aus dem Kern entfernt —
> ersatzlos, wie unten begründet; `heater.py` und `gym_bridge.py` sprechen
> den `exec`-Kanal, ein echtes Modbus-Gerät erreicht man über einen
> mbpoll-Wrapper (`examples/machine/openplc/setup.sh`).

## Die Portgrenze: der Kanal

Ein **Kanal** ist das gesamte Vokabular — Name, Bereich, und wohin er bei
Kontaktverlust geht. Nicht eine Funktion pro Peripherie: diese Form hat drei
Backends je zwei Fan-Funktionen tragen lassen, für ein Gerät, das kein Executor
je angesteuert hat.

Eine neue Maschine ist eine **neue Tabelle, kein neuer Code**. Das ist die
Eigenschaft, um die dieses Modul gebaut ist, und alles Weitere unten dient nur
dazu, sie auch dann zu behalten, wenn die Maschine kein Modbus spricht.

## Was unter dem Kanal liegt: der Transport

Von allem, was ein Kanal tut, ist nur ein schmaler Streifen protokollabhängig:
die Bytes auf der Leitung. Namen, Bereiche, der sichere Wert, der Watchdog, die
Ablehnung — alles das liegt darüber und ist von der Maschine unabhängig.

Also wandert der Streifen nach draußen. geistshell kennt genau **einen**
Transport:

```
lesen:     execve(programm)                → eine ganze Zahl auf stdout, Exit 0
schreiben: execve(programm, "<wert>")      → Exit 0 = angenommen
```

Mehr ist es nicht. Ein Sensor ist ein Programm, das eine Zahl druckt. Ein Aktor
ist ein Programm, das eine Zahl entgegennimmt. Damit ist **jedes** Gerät
anschließbar — I²C, MQTT, HTTP, ein `cat` auf sysfs, ein Python-Dreizeiler —
ohne dass geistshell je wieder angefasst wird.

### Der Vertrag, vollständig

| | Lesen | Schreiben |
|---|---|---|
| `argv` | `[programm]` | `[programm, "<wert>"]` |
| Erfolg | Exit 0, stdout = eine ganze Zahl in Registereinheiten | Exit 0 |
| Fehler | Exit ≠ 0, unparsbare Ausgabe, Timeout → `SPG_E_IO` | Exit ≠ 0, Timeout → `SPG_E_IO` |
| Timeout | `SPG_DEVICE_TIMEOUT_MS`, dann Prozessgruppen-Kill | dasselbe |
| stderr | ins Journal, nie in den Kontext | dasselbe |

Drei Festlegungen, die nicht offensichtlich sind:

- **Kein Shell.** `execve` direkt, `argv` aus der Config, der Wert als eigenes
  Argument. Der vom Modell gewählte Sollwert wird **nie** Teil einer
  Kommandozeile, die ein Interpreter liest. Damit gibt es keine Injektion, die
  irgendjemand vergessen könnte zu quoten.
- **Ganze Zahlen in Registereinheiten**, wie bisher. 23,5 °C ist die Zahl 235.
  Ein Skalierungsfaktor wäre ein zweiter Ort, an dem eine falsche Zahl wohnen
  kann — und Fließkomma bräche den byteidentischen Replay.
- **Echo bleibt möglich, aber freiwillig.** Druckt das Schreibprogramm eine
  Zahl, wird sie mit dem befohlenen Wert verglichen; schweigt es, gilt Exit 0.
  Die Eigenschaft aus dem Modbus-Stand geht damit nicht verloren, sie wird nur
  zur Sache des Programms, das die Maschine tatsächlich kennt.
- **Der Fork ist billig genug.** Ein `fork`+`exec` kostet unter einer
  Millisekunde, die Regelschleife läuft in Sekunden. `spg_cmd_executor_run`
  liefert die gebundene Ausführung bereits fertig: Timeout, `setrlimit`,
  Prozessgruppen-Kill.

### Warum das nicht MCP ist

MCP löst dieselbe Aufgabe — ein fremdes Programm besitzt die Maschine — und
wäre trotzdem der teurere Weg. Drei Gründe, in der Reihenfolge ihres Gewichts:

1. **Das constrained decoding.** Das Modell hier ist klein und lokal. Es
   liefert eine gültige Aktion nicht, weil es sie versteht, sondern weil
   `grammar_mask.c` ihm pro Token nur die Zeichen erlaubt, die zu einer
   gültigen Aktion führen; die Struktur drumherum wird wörtlich eingesetzt statt
   generiert. Das trägt, solange das Vokabular klein und beim Start bekannt ist:
   neun Verben, eine Handvoll Kanalnamen. MCP-Tools kommen mit beliebigen
   JSON-Schemas — verschachtelte Objekte, optionale Felder. Die Maske zu behalten
   hieße, einen Compiler von JSON-Schema nach Token-Maske zu bauen. Das ist ein
   eigenes Projekt, und `Small-Model-Gap.md` existiert, weil freies Dekodieren
   schon einmal wehgetan hat.
2. **Die Sicherheitsprüfungen wandern mit.** `min`/`max`/`safe` liegen heute in
   geistshell, in reinem Code, testbar ohne jede Maschine. Als MCP-Werkzeug
   lägen sie im Server, und in der Policy stünde „darf `set_valve` rufen" statt
   „darf 0 bis 100 schreiben". Eine Prüfung, die man nicht mehr sieht, ist eine
   Prüfung, die man nicht mehr prüft.
3. **Werkzeugbeschreibungen sind Prompttext.** Ein MCP-Server liefert
   Beschreibungen, die in den Kontext gehen. Dieselbe Codebasis wirft
   Prozess-Kommandozeilen aus dem Kontext, um genau diese Fläche zu vermeiden
   (`machine_state.h`).

Und es ist keine Sackgasse: ein MCP-Server wird über eine **Brücke** erreicht,
die selbst ein `exec`-Kanal ist.

```
(channel (name "druck") (program "/opt/geistshell-mcp-bridge") (range 0 4000))
```

geistshell lernt nie JSON-RPC. Wer es braucht, schreibt ein Programm — so wie
für jedes andere Gerät auch.

### Warum Modbus ausgebaut wird

Modbus TCP hat gewonnen, was es gewinnen sollte: den Beweis, dass **derselbe
Codepfad** eine simulierte und eine echte Maschine fährt. Drei fremde Maschinen
liefen ohne eine Zeile C in geistshell. Das Argument ist erledigt, nicht
widerlegt.

Was bleibt, ist ein Protokoll im Kern eines Programms, das behauptet,
maschinenunabhängig zu sein.

Es wird **ersatzlos entfernt**. Kein Begleitprogramm, kein `geistshell-modbus`:
Modbus-Kommandozeilenwerkzeuge gibt es seit Jahrzehnten (`mbpoll` und
Verwandte), und ein eigenes zu schreiben hieße, dieselbe Wartungslast unter
neuem Namen zu behalten. Ein Kanal ist dann ein Dreizeiler:

```sh
#!/bin/sh
# /opt/plant/heater — Halteregister 1 auf 10.0.0.5
# ohne Argument lesen, mit Argument schreiben. Flags nach Werkzeug.
[ $# -eq 0 ] && exec modbus-read  10.0.0.5 1
exec modbus-write 10.0.0.5 1 "$1"
```

```
(channel (name "temp")   (program "/opt/plant/temp")   (range -400 9000))
(channel (name "heater") (program "/opt/plant/heater") (range 0 100) (safe 0))
```

Das ist der Punkt, an dem der Entwurf sich auszahlt: der „Treiber" für ein
ganzes Industrieprotokoll ist ein Shell-Skript, das jemand einmal schreibt und
das niemand in diesem Repo pflegt. Danach steht in geistshell **kein einziges
Geräteprotokoll** mehr — nur noch die Tabelle, die Bereiche, der sichere Wert
und der Watchdog. Die Dinge, die zu prüfen sind, ohne die Dinge, die zu sprechen
sind.

Ehrlich benannt, was das kostet: der Vergleich der Transaktions-ID und der
Echo-Vergleich liegen heute in getestetem C. Der Echo-Vergleich kommt über den
freiwilligen Rückgabewert zurück. Die Transaktions-ID ist Modbus-Semantik und
wird zur Eigenschaft des Werkzeugs — bei `mbpoll` und Verwandten ist sie
vorhanden, aber sie ist nicht mehr etwas, das dieses Repo garantiert. Wer sie
garantiert braucht, prüft das Werkzeug, nicht geistshell.

## Die Konfiguration: ein Kanal ist eine Form, kein Doppelpunktstring

`name:reg:min:max:w:safe` trug genau so lange, wie ein Kanal eine Registernummer
war. Ein Programmpfad passt nicht mehr hinein — und `device.h` hat den Umzug
seit dem ersten Tag angekündigt.

Also dieselbe Sprache wie Policy und Simulator: S-Expressions, geladen über
`sexpr.c` und geprüft über `schema.c`. Kein zweiter Parser, keine zweite
Angriffsfläche, keine zweite Sache, die byteidentisch bleiben muss.

```
(device
  (channel (name "temp")
           (program "/opt/plant/read-temp")
           (range -400 9000))
  (channel (name "heater")
           (program "/opt/plant/heater")
           (range 0 100)
           (safe 0)))
```

**`(safe …)` vorhanden heißt schreibbar.** Ein eigenes `w`-Flag daneben wäre
eine zweite Quelle für dieselbe Aussage, und die Regel „ein schreibbarer Kanal
muss einen sicheren Wert deklarieren" wird so strukturell unverletzbar statt
nachträglich geprüft. Der Preis ist benannt: ein `(safe …)` an einem Sensor
macht ihn still schreibbar. Der Bereich begrenzt ihn weiterhin, und ein Sensor
mit einem sicheren Wert ist ein Tippfehler, den ein Review sieht.

**`(network true)` deklariert den Transportbedarf (#119).** Ein Kanal, dessen
Programm über das Netz spricht — MQTT, HTTP, Modbus TCP, eine MCP-Bridge —
trägt das Feld in seiner Form. Es ist **Operator-Trust**: der Policy-Gate
leitet die Netzwerkentscheidung eines `device_write` aus diesem Feld der
geladenen Tabelle ab, niemals aus Modelltext (die Recommendation-Form muss
`uses_network false` sagen, sonst scheitert der Parse). Unter
`(network_default deny)` wird ein Netzwerk-Kanal vor jedem Fork verweigert
und journalisiert; lokale Kanäle bleiben nutzbar. Fehlt das Feld, gilt lokal;
alles außer den Symbolen `true`/`false` ist ein Schemafehler. geistshell kann
nicht prüfen, was ein Programm zur Laufzeit tut — die wahrheitsgemäße Angabe
ist Verantwortung des Betreibers (siehe SECURITY.md).

`--device-channel` bleibt für Einzeiler und Tests bestehen — es nimmt
dieselbe `(channel …)`-Form als ein Argument, kein zweites Format.

## Bereich heißt Ablehnung, nicht Klemmung

Ein Wert außerhalb des Bereichs wird **abgelehnt und nicht gesendet**
(`SPG_E_LIMIT`). Klemmen führt einen Beinahe-Treffer eines bereits falschen
Befehls aus — so gehen Maschinen kaputt an Software, die hilfsbereit sein
wollte. Der Aufrufer erfährt, dass sein Wert verworfen wurde.

**Jede Ablehnung wird vor dem Fork entschieden.** Kanal unbekannt,
schreibgeschützt, außerhalb des Bereichs: alles testbar, ohne dass je ein
Programm startet. Eine Sicherheitsprüfung, die erst greift, wenn eine Maschine
angeschlossen ist, wird an dem Tag zum ersten Mal ausgeführt, an dem sie
gebraucht wird.

Der Transportwechsel ändert an diesem Absatz nichts — und das ist der Beleg
dafür, dass die Naht an der richtigen Stelle liegt. Was oberhalb des Transports
steht, hat den Transport nicht bemerkt.

- **Timeout in beide Richtungen.** Eine Maschine, die schweigt, darf die
  regelnde Schleife nicht anhalten. Beim Socket war das `SO_RCVTIMEO`, beim
  Programm der Prozessgruppen-Kill; die Frist ist dieselbe.

- **Eine Runde, eine Frist (#121).** `spg_device_sample` startet alle
  Kanalprogramme **gleichzeitig** als einen Batch über `spg_cmd_executor_run`.
  Die Worst-Case-Latenz einer Abtastrunde ist damit eine Frist plus
  Spawn-Overhead — nicht eine Frist pro Kanal: 32 tote Sensoren blockieren
  ~1 s, nicht ~32 s. Default `SPG_DEVICE_TIMEOUT_MS` = 1000 ms; konfigurierbar
  pro Gerät über `sample_timeout_ms` bzw. `--device-sample-ms` (Maximum: der
  Batch bleibt durch die Frist selbst gebunden, ein Wert oberhalb des
  Run-`wall_ms` wäre sinnlos — die Runde soll das Wall-Budget nie um ein
  Kanalzahl-Vielfaches überschreiten können). Nachzügler werden mit ihrer
  Prozessgruppe getötet und rendern `unknown`; fertige Messungen bleiben
  erhalten; die Ausgabereihenfolge ist die Tabellenreihenfolge, unabhängig von
  der Fertigstellungsreihenfolge. Feste Stack-Puffer, keine Allokation.

## Der Sensor: der fehlende Rückweg

**Jeder Kanal wird pro Schritt abgetastet und gerendert** — einmal vor dem
ersten Tick, damit die erste Entscheidung über eine gesehene Anlage fällt, und
danach nach jeder Aktion, an derselben Stelle wie die Host-Telemetrie: der
nächste Tick muss über die Maschine urteilen, die die Aktion hinterlassen hat.

Der Block braucht keinen eigenen Journaleintrag. Der Actor schreibt den
kompletten gerenderten Kontext als `SPG_JOURNAL_EVENT_MODEL_INPUT` in die
Hash-Kette: was der Agent sah, steht damit im selben Beleg wie das, was er tat.

```
(device-state (temp 2350) (heater 0))
```

Kein `device_read`-Verb. Das hier ist eine Regelschleife; ein Regler, der einen
Schritt verbrennt, um zu messen, hat die halbe Frequenz. Und Lesen hat nichts zu
entscheiden, also gehört es nicht durch das Policy-Gate — es gehört in die
Wahrnehmung, wo die Host-Telemetrie schon liegt.

Ein Kanal, dessen Programm scheitert, rendert `unknown`, wie jedes andere
unlesbare Feld in `machine_state.h`. Ein fehlender Wert darf nie als Null
ankommen.

### Warum Pull und nicht Push

Abgetastet wird **holend**: einmal pro Tick, an derselben Stelle, an der
`spg_machine_sample` schon die Host-Telemetrie holt. Kein Daemon, kein Socket,
kein Signal.

**Signale scheiden zuerst aus.** Sie koaleszieren — fünf Ereignisse zwischen
zwei Aufrufen werden eine Zustellung — und tragen keine Nutzlast. Man erfährt
„irgendetwas hat sich geändert" und muss trotzdem lesen. Dazu ein Handler in
einer `malloc`-freien Codebasis, der fast nichts darf. Mehr Komplexität für
weniger Information als ein `read()`.

**Push über FIFO oder Socket ist ernsthafter und kostet drei Eigenschaften:**

1. **Determinismus.** Der gesehene Wert hinge davon ab, *wann* der Sensor
   geschrieben hat; zwei Läufe derselben Anlage sähen verschiedene
   Verschränkungen. Das ist der Fehler des `--speed`-Bugs weiter unten, eine
   Ebene höher: eine Messung, die von der Wanduhr abhängt, misst die Wanduhr mit.
2. **Wer initiiert.** Bei Pull entscheidet geistshell, wann und wie oft; ein
   defekter Sensor verschwendet nur sein eigenes Timeout. Push wirft vier Fragen
   auf, die im Pull-Modell nicht entstehen: wer darf schreiben, was bei einem
   flutenden Schreiber, welcher Wert gilt bei dreien seit dem letzten Schritt,
   und ob der alte noch gültig ist, wenn keiner kam.
3. **Sichtbarkeit des Ausfalls.** Bei Pull ist ein totes Programm ein Timeout und
   damit `unknown`. Bei Push ist es Stille — nicht unterscheidbar von einem Wert,
   der sich nicht geändert hat. Ein Sensor, der stirbt und dabei aussieht wie ein
   konstanter Messwert, ist die gefährlichste Fehlerart, die ein Regler haben
   kann.

Dazu strukturell: die Schleife ist einteilig und ohne Threads. Push hieße eine
Event-Schleife oder ein nebenläufiger Drain — für einen Effizienzgewinn, den es
nicht gibt.

**Die Zahlen.** Ein Schritt besteht zum weitaus größten Teil aus dem
Modell-Decode, 200 ms bis mehrere Sekunden. Ein `fork`+`exec` eines kleinen
C- oder Shell-Programms kostet unter einer Millisekunde; zehn Kanäle sind
damit ein bis drei Prozent eines Schritts.

Der eine Ort, an dem Pull real weh tut, ist ein Sensor als **Python-Skript**:
20–50 ms Interpreterstart, bei zehn Kanälen 300 ms pro Schritt. Das ist ein
Problem der Sensorprogramme, kein Problem des Mechanismus. Der Ausweg ist ein
Shell-Einzeiler (`cat /sys/class/thermal/thermal_zone0/temp`) — und wenn das
nicht reicht, ein Programm für alle Kanäle statt eines pro Kanal:

```
(device (source "/opt/plant/readall") …)   ; ein exec, druckt "temp 2350\nheater 0\n"
```

Das kollabiert N Forks auf einen und hat einen zweiten, wichtigeren Vorteil:
alle Werte stammen aus **demselben Augenblick**. Temperatur bei t und Druck bei
t+30 ms ist ein inkonsistenter Schnappschuss, auf dem ein Regler dann eine
Entscheidung baut.
ponytail: noch nicht gebaut. Fällig, wenn ein Schritt spürbar am Abtasten hängt
oder ein Regler zwei Kanäle gegeneinander rechnet.

### Das Ereignis zwischen zwei Schritten

Der Fall, in dem Pull tatsächlich verliert: ein Not-Halt, ein Endschalter, ein
Alarm von 50 ms Dauer. Ein Tick pro Sekunde sieht ihn nicht.

Die Antwort ist trotzdem nicht Push, sondern ein **rastendes Sensorprogramm**:
es merkt sich das Ereignis, meldet es beim nächsten Lesen und löscht sich dann.
Die Latenzanforderung wandert dorthin, wo sie hingehört — in das Programm an der
Hardware, das in Mikrosekunden reagieren kann, statt in einen Agenten, der auf
einen Decode wartet.

Es ist dieselbe Form, die `heater.py` weiter unten schon hat: die
Übertemperaturabschaltung **rastet** und muss zurückgesetzt werden. Eine Ebene
tiefer, dasselbe Argument.

Und der Satz, der über allem steht: ein Agent, der auf ein Sprachmodell wartet,
ist **niemals** der Ort für eine Sicherheitsabschaltung. Die gehört in die SPS
oder in einen Schaltkreis. Der Watchdog hier ist die letzte Verteidigungslinie,
nicht die erste — er fährt in den sicheren Zustand, wenn der Kontakt abreißt,
und ersetzt keine Verriegelung.

## Der Watchdog

Jeder schreibbare Kanal **muss** einen sicheren Wert deklarieren. Ohne ihn träfe
die Entscheidung bei Kontaktverlust der Wert, den die Maschine zuletzt gehört
hat — der Zustand mit der geringsten Wahrscheinlichkeit, sicher zu sein.

Der Watchdog wird **vor** dem Schreiben geprüft, nicht danach. Eine Maschine,
die nicht mehr antwortet, ist eine Maschine mit unbekanntem Zustand; der einzige
Befehl, der dann noch versucht gehört, ist der sichere Zustand — nicht der
nächste Sollwert, den ein Modell gewählt hat.

`spg_device_safe_state` versucht **alle** Kanäle und meldet den ersten Fehler.
Eine Anlage, die auf halbem Weg in den sicheren Zustand stehenbleibt, ist
schlimmer als beide Endzustände.

**Die Einheit ist die des Aufrufers.** Es gibt keine richtige feste Einheit: das
CLI-Kommando übergibt Nanosekunden, der Agenten-Loop rechnet in Schritten
(`step + 1`), und genau das macht seinen Replay deterministisch. Eine
Millisekunden-Frist gegen einen Schrittzähler zu prüfen wäre eine Zahl, die wie
Zeit aussieht und keine ist. Deshalb heißt das Flag `--device-watchdog-steps`.

Mit `exec` bekommt der Watchdog eine Aufgabe mehr: ein Programm, das dauerhaft
mit Timeout stirbt, ist Kontaktverlust — genauso wie ein stummer Socket.

**Pro Kanal, pro Tick (#118).** Kontakt wird pro Kanal geführt: eine
erfolgreiche Transaktion füttert genau den Kanal, über den sie lief, und
zusätzlich den globalen Stempel. Abgelaufen ist der Watchdog, sobald der
globale Stempel **oder irgendein schreibbarer Kanal** eine volle Frist ohne
Kontakt ist — ein Sensor, der weiter antwortet, kann einen verstummten Aktor
nicht mehr verdecken. (Reine Sensor-Tabellen laufen wie zuvor über den
globalen Stempel: Lesen ist dort der einzige Kontakt, den es gibt.)

Geprüft wird nicht mehr nur vor einem `device_write`:
`spg_device_watchdog_service` läuft in jedem Agenten-Tick — auch in Ticks, die
nur beobachten, Memory schreiben oder `finish` wählen — und noch einmal vor dem
regulären Run-Ende. Beim Ablauf fährt er alle schreibbaren Kanäle best-effort
auf ihren sicheren Wert und journaliert
`(device_watchdog (outcome expired) (safe_state ok|failed))`. Ein anhaltender
Ablauf wird **einmal** behandelt (Latch), nicht jeden Tick erneut;
wiederkehrender Kontakt spannt den Latch neu.

## Die Aktion

`SPG_ACTION_DEVICE_WRITE` — die erste **nicht umkehrbare** Aktion im
geschlossenen Aktionsraum. Der Kommentar am Enum verlangte dafür „eine stärkere
Geschichte als 'die Policy hat ja gesagt'". Sie besteht aus drei Teilen:

1. Die Kanaltabelle begrenzt sie. Ein Wert außerhalb wird abgelehnt, bevor
   überhaupt ein Prozess startet — das Modell kann keine Zahl verlangen, die der
   Betreiber nicht freigegeben hat.
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

### Warum das Verb geschlossen bleibt

Die Versuchung, eine neue Maschine zu einer neuen Aktionsart zu machen, wird
durch drei `switch`-Blöcke bestraft: `kinds_for_cap` und `spg_scaffold_for_kind`
in `grammar_mask.c` sowie die Kostenzuordnung in `policy.c`. Genau diese
Vollständigkeitspflicht hat den Fehler aus #66 gefunden, als #75 ein drittes
Kind hinzufügte — `-Wswitch` sieht, was ein Registry zur Laufzeit nicht sieht.

Ein Aktor ist eine Zeile in der Tabelle, ein Protokoll ist ein Programm. Ein
neues Verb ist nur dann fällig, wenn eine Aktion sich nicht als „eine Zahl auf
einen benannten Kanal" ausdrücken lässt — und diese Aktion gibt es bisher nicht.

## Die simulierte Anlage

`examples/machine/plant/heater.py` — ein beheizter Kessel mit Wärmeträgheit,
Verlust an die Umgebung und **rastender Übertemperaturabschaltung** bei 90 °C.
Nur Standardbibliothek.

Die Abschaltung ist der Punkt. Eine Anlage, die nur warm wird, belohnt jeden
Regler, der die Heizung aufreißt, und misst damit nichts. Diese hier sperrt sich
aus und muss zurückgesetzt werden — sie bestraft genau den Regler, der handelt,
ohne vorher zu schauen.

Zwei Modellfehler, beide beim ersten Lauf gefunden:

1. **Der Trip war unerreichbar.** Bei 100 % lag das Gleichgewicht bei 50 °C, die
   Abschaltung bei 90 °C. Eine Grenze, die keine Eingabe erreichen kann, ist
   Dekoration. Jetzt liegt das Gleichgewicht bei 145 °C, und alles über ~56 %
   rastet irgendwann aus.
2. **Der Simulator hat die Messung gestört.** `--speed` erhöhte ursprünglich die
   Tickrate — bei Faktor 40 waren das 800 Wakeups pro Sekunde, genug, um den
   lastempfindlichen `test_cli_machine_run` in derselben Suite kippen zu lassen.
   Jetzt skaliert `--speed` die Physik pro Tick; die Rate bleibt bei 20 Hz. Eine
   Testanlage, die die gemessene Maschine beeinflusst, ist keine Fixture.

## Drei Maschinen, ein Codepfad

| Maschine | Was sie beisteuert | Wer sie geschrieben hat |
|---|---|---|
| `heater.py` | Physik mit rastender Verriegelung | ich — deshalb als Prüfstand wertlos |
| OpenPLC | echte IEC-61131-3-Runtime, fremder Modbus-Stack und Adressraum | fremd, aber **ohne Physik**: ein Regler, keine Anlage |
| `gym_bridge.py` | fremde Physik, fremde Schwierigkeit, veröffentlichte Baselines | fremd |

**geistshell hat für keine davon eine Zeile C gebraucht.** Das war der Beweis,
den der Modbus-Einstieg liefern sollte. Nach dem Umzug gilt derselbe Satz für
jedes Protokoll, nicht nur für dieses eine.

### Die Gymnasium-Brücke

```
python3 -m venv build/gymenv
build/gymenv/bin/pip install "gymnasium[classic-control]"
build/gymenv/bin/python examples/machine/plant/gym_bridge.py --env Pendulum-v1 --port 5502
```

Ein venv, kein `pip install --break-system-packages`: PEP 668 sperrt die
System-Python aus gutem Grund, und ein Beispiel darf die Installation des
Nutzers nicht gefährden. Fehlt das venv, **überspringt** der Test sich, statt rot
zu werden — ein frischer Checkout darf kein `pip install` verlangen.

Drei Entwurfsentscheidungen, die nicht offensichtlich sind:

- **Ein eigenes Commit-Register.** Eine RL-Umgebung schreitet nur voran, wenn man
  handelt, und eine mehrdimensionale Aktion kommt Register für Register an.
  „Schreite fort, wenn das letzte Aktionsregister geschrieben wurde" macht die
  Bedeutung eines Schreibvorgangs davon abhängig, welches es war — für Pendulums
  eine Dimension egal, für einen Quadrocopter falsch. Ein zusätzlicher Roundtrip
  kauft eine Schnittstelle ohne Mehrdeutigkeit.
- **Sättigen, nicht überlaufen.** Beobachtungen sind Fließkomma, Register sind 16
  Bit. Ein Wert, der in eine plausible kleine Zahl überläuft, ist schlimmer als
  einer, der an der Grenze klebt — der erste ist still falsch. Register 105 zählt
  die Sättigungen, damit eine schlecht skalierte Kanaltabelle sichtbar wird statt
  rätselhaft.
- **Bei jedem Reset geseedet**, nicht einmal beim Start: zwei Läufe desselben
  Skripts müssen dieselbe Episode sehen, sonst beweist ein Journal-Replay nichts
  über die Entscheidungen darin.

Und ein glücklicher Zufall: Gymnasium-Umgebungen schreiten **auf Befehl** fort,
nicht nach Wanduhr. Der Agenten-Loop rechnet in `step + 1`, der Watchdog misst
deshalb in Schritten. Das passt exakt — die Wanduhr-Physik des Heizers war die
Fehlanpassung, nicht die Regel.

## Der Weg dorthin

| Schritt | Was | Warum in dieser Reihenfolge |
|---|---|---|
| 1 | `(device-state …)` in den Kontext | Unabhängig vom Transport, und ohne ihn ist jeder Sensor unsichtbar |
| 2 | `exec`-Transport neben Modbus | Beide gleichzeitig lauffähig, die Tests vergleichen sie an derselben Anlage |
| 3 | Sexpr-Konfiguration | Fällig, sobald ein Programmpfad in den Kanal muss |
| 4 | Modbus-Wrapper für `heater.py`, OpenPLC und die Gym-Brücke | Die drei bestehenden Anlagen müssen über `exec` genauso laufen wie vorher |
| 5 | Modbus aus `device.c` entfernen | Löschen zuletzt, nie auf Verdacht |

Schritt 5 ist der einzige Punkt, an dem etwas verloren gehen kann. Er kommt
deshalb nach einem Lauf, der beide Transporte an `heater.py` gegeneinander
gestellt hat — und die Wrapper aus Schritt 4 sind zugleich die Beispiele, die
das Repo danach als einzige Modbus-Spur behält.

## Was bewusst noch fehlt

Skalierung pro Kanal, Kanäle, die mehr als eine Zahl tragen, und mehrere
Maschinen pro Lauf. Alles davon wartet auf eine zweite echte Anlage — bis dahin
wäre es Struktur ohne Aufrufer, und wie die kostet, steht oben in diesem
Dokument.
