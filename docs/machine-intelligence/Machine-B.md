# Machine B — Phase 15

Trägt die Architektur eine zweite Maschine, ohne dass der Agent-Kern für sie
umgebaut wird?

Ticket: geisten/geistshell#75.

## Machine B ist echt

Kein Simulator: der Pi 5 hat einen PWM-Lüfter als hwmon-Gerät.

| Kanal | Quelle | Rolle |
|---|---|---|
| Temperatur | `/sys/class/thermal/thermal_zone0/temp` | Sensor |
| Drehzahl | `pwmfan/fan1_input` (gemessen: 9950 U/min) | Sensor |
| Duty | `pwmfan/pwm1` (0..255) | **Aktor** |

Kein Heizelement — CPU-Last ist die Heizung, und die Workloads aus #68
erzeugen sie. Der Unterschied zu Machine A ist der, auf den es ankommt:
Prozesse pausieren ist diskret und wirkt sofort, einen Lüfter zu drehen ist
kontinuierlich und wirkt mit Verzögerung.

## Kalibrierung ist Konfiguration

`struct spg_thermal_calibration` trägt Sensor-Offset, Mindest- und
Maximal-Duty, einen **Boden** und einen Mindestabstand zwischen Änderungen.
Nichts davon steht als Konstante im Code: ein realer Lüfter folgt seinem
Tastverhältnis nicht linear, ein realer Sensor liest daneben, und keine zwei
Boards sind sich einig. Fest verdrahtete Zahlen wären für genau eine Maschine
richtig — der Fehlermodus, den diese Phase prüft.

Der **Boden** ist eine Sicherheitseigenschaft: der Agent darf den Lüfter
verlangsamen, nie abschalten. Ihn abzuschalten wäre eine erlaubte Aktion, die
die Platine gart.

Ein eigener Fehler dabei, vom eigenen Test gefunden: die Klemmung stand
zunächst im `#if defined(__linux__)`-Zweig. Damit existierte die
Sicherheitseigenschaft auf jeder anderen Plattform nicht — und ließ sich auf
der Entwicklungsmaschine nicht einmal testen. Sie liegt jetzt außerhalb des
Plattformzweigs, weil sie eine Entscheidung ist und kein I/O-Detail.

## Machine Integration Effort

Gemessen, nicht geschätzt. Für **eine** Aktion (`machine_set_fan`):

| Art | Dateien | Zeilen |
|---|---|---|
| **Neu, maschinenspezifisch** | `thermal.h`, `thermal.c` | 208 |
| **Core geändert** | `policy.h`, `policy_config.h`, `policy.c`, `recommendation.c`, `grammar_mask.c`, `cli/main.c` | **37** |
| Test | `test_machine_thermal.c` | 96 |
| Nur Konfiguration | – | 0 |

**Sechs Core-Dateien für eine Aktion.** Der Adapter selbst ist sauber getrennt
und enthält nichts vom Agenten; aber der Action Space ist ein geschlossenes
Enum, und jedes `switch` darüber muss den neuen Kind lernen.

### Was der Compiler dabei gefunden hat

`-Wswitch` meldete beim Hinzufügen des dritten Machine-Kinds, dass
`grammar_mask.c` die **beiden ersten nie kannte**. Die Machine-Actions aus #66
waren dem constrained Decoder seit ihrer Einführung unbekannt — er konnte sie
gar nicht anbieten. Aufgefallen ist das erst, weil eine zweite Maschine einen
dritten Fall hinzufügte.

Ein geschlossenes Enum ist nur geschlossen, wenn jedes `switch` darüber
vollständig ist. Hier war es das nicht, und niemand hat es bemerkt.

### Der Parameter im String

`(kind machine_set_fan) (target "fan:60")` — die Lüfterstufe reitet in der
vorhandenen `target`-Zeichenkette mit, statt ein eigenes Grammatikfeld zu
bekommen.

Das ist ein bewusster Kompromiss **und ein Befund**: eine parametrisierte
Aktion für eine zweite Maschine ohne Grammatikänderung unterzubringen heißt,
den Parameter in einen String zu kodieren. Die nächste Maschine mit zwei
Parametern hat diese Ausrede nicht mehr.

## GO / PIVOT

Das Ticket gibt GO, *wenn Machine B überwiegend über Adapter, Schema und
Konfiguration integrierbar ist*.

**Das ist nicht der Fall.** Null Zeilen reine Konfiguration, 37 Zeilen Core in
sechs Dateien, und eine latente Lücke, die nur auffiel, weil der Compiler beim
dritten Fall nachhakte.

Die Warnung des Tickets — *wenn jeder Maschinentyp tiefe Core-Änderungen
braucht, droht ein projektspezifisches Framework statt einer Plattform* — ist
damit **nicht widerlegt**. Sie ist auch nicht bestätigt: 37 Zeilen sind keine
tiefe Änderung, und die Governance-Schicht selbst (Policy Gate, Budget,
Journal, Replay, Eval) hat Machine B **ohne jede Änderung** getragen. Das ist
der eigentlich tragfähige Teil.

Die ehrliche Zwischenbilanz für #77: **die Governance generalisiert, der Action
Space nicht.** Wer eine dritte Maschine anschließt, zahlt denselben Preis
wieder — und ihn zu senken hieße, Action Kinds datengetrieben zu machen statt
als Enum. Das ist eine eigene Entscheidung und gehört nicht in diese Phase.

## Was fehlt

**Der Executor ist nicht verdrahtet.** Grammatik, Policy, Capability, Budget
und Maske kennen `machine_set_fan`; der Orchestrator führt ihn noch nicht aus.
Für die Messung der Integrationskosten war das nicht nötig — der Dispatch-Zweig
ist eine weitere Core-Datei und würde die Zahl erhöhen, nicht ihre Aussage
ändern.

**Das Ratenlimit ist Konfiguration ohne Durchsetzung.**
`min_interval_ticks` steht in der Kalibrierung, aber niemand liest es. Ein
Agent, der jeden Tick neu entscheidet, würde oszillieren.

**Kein Lauf auf echter Hardware mit Aktor.** `pwm1` zu schreiben braucht Root;
die Leseseite ist auf dem Pi verifiziert, die Schreibseite nicht.
