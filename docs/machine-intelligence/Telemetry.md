# Telemetry — Phase 1

Read-only Systemtelemetrie als typed Snapshot. Der Agent kann die Maschine
**beobachten**, aber nichts verändern — kein Action Kind, keine Verdrahtung in
den Context (das ist Phase 3, geisten/geistshell#63).

Ticket: geisten/geistshell#61. Baseline: [Baseline.md](Baseline.md).

## Drei Schichten

| Schicht | Datei | Eigenschaft |
|---------|-------|-------------|
| 1 — OS lesen | `src/machine/telemetry_host.c` | einzige Stelle mit I/O, Linux-spezifisch |
| 2 — Normalisieren | `src/machine/telemetry.c` | reine Funktionen über Puffer, kein I/O, keine Uhr |
| 3 — Serialisieren | `src/machine/telemetry.c` | deterministische S-Expression |

Die Trennung ist der Grund, warum die Tests auf jedem Host laufen: sie füttern
die Parser mit statischen Fixtures und fassen `/proc` nie an. Kein Testergebnis
hängt von CPU-Last, Temperatur oder Plattform der CI-Maschine ab.

## Schema

```
(machine-state (cpu-load-bp 9200) (load-1-cbp 175) (memory-total-bytes 1024)
 (memory-used-bytes 512) (swap-used-bytes 0) (temperature-mc 78400)
 (cpu-freq-khz 1500000) (throttle none) (process-count 137))
```

| Feld | Einheit | Quelle | Fehlt wenn |
|------|---------|--------|------------|
| `cpu-load-bp` | Basispunkte 0..10000 | `/proc/stat`, Delta zweier Samples | erster Tick, Δ=0, Counter-Reset |
| `load-1-cbp` | Load × 100 | `/proc/loadavg` | Datei fehlt/malformed |
| `memory-total-bytes` | Bytes | `/proc/meminfo` `MemTotal` | Datei fehlt |
| `memory-used-bytes` | Bytes | `MemTotal − MemAvailable` | `MemAvailable` fehlt |
| `swap-used-bytes` | Bytes | `SwapTotal − SwapFree` | eines der beiden fehlt |
| `temperature-mc` | Milligrad Celsius | `/sys/class/thermal/thermal_zone0/temp` | kein Thermal-Zone-Sensor |
| `cpu-freq-khz` | kHz | `.../cpu0/cpufreq/scaling_cur_freq` | kein cpufreq-Treiber |
| `throttle` | `none`/`active`/`past`/`unknown` | hwmon `rpi_volt`, sonst `.../soc:firmware/get_throttled` | weder hwmon-Gerät noch Firmware-Node |
| `process-count` | Anzahl | numerische Einträge in `/proc` | `/proc` nicht lesbar |

Alle Werte sind **Ganzzahlen im Fixpunkt**. Keine Floats: deren Formatierung
hängt von der Locale ab und deren Rundung vom Modus — beides würde
byte-identische Ausgabe zerstören.

`MemAvailable`, nicht `MemFree`: nur ersteres sagt, was ein Workload
tatsächlich bekommen kann.

## Unbekannt ist ein Wert, keine Null

Fehlende Werte werden als Symbol `unknown` serialisiert und intern als
Sentinel (`SPG_MACHINE_UNKNOWN` = `UINT64_MAX`, `SPG_MACHINE_UNKNOWN_S` =
`INT64_MIN`) geführt.

Das ist keine Kosmetik. Ein fehlender Load Average, der als `0` durchgereicht
wird, liest sich für ein Modell wie eine vollkommen idle Maschine — und genau
diesen Fehler hat der Test `render_unknown` während der Entwicklung gefunden:
Der Snapshot initialisierte den Load nicht auf `unknown`, ein fehlgeschlagener
`/proc/loadavg`-Parse hätte „Last 0.00" gemeldet.

Der Sentinel-Ansatz hat eine dokumentierte Grenze: ein echter Wert von exakt
`UINT64_MAX` wäre nicht unterscheidbar. Für Bytes, kHz und Prozentwerte ist das
physikalisch ausgeschlossen.

## Keine Uhr im Modul

`spg_machine_sample()` bekommt `timestamp_ns` vom Aufrufer und liest **nie**
`clock_gettime`. Grund steht in [Baseline.md](Baseline.md#2-determinismus-inventar):
Das Journal hasht seinen Zeitstempel in die Record-Chain, die CLI reicht einen
synthetischen Tick-Zähler ein, und der einzige Clock-Read der Runtime
(`cmd_executor.c:50`) verlässt sein Modul nicht. Eine zweite Zeitquelle würde
den Freeze aus Phase 0 brechen, sobald ein Telemetriewert in Context oder
Journal landet.

Deshalb ist auch die CPU-Auslastung nicht im Sampler versteckt: sie ist das
Delta zweier Samples, und der Aufrufer hält das vorherige. `spg_machine_state`
trägt die rohen Counter mit, damit der nächste Aufruf sie direkt einspeisen
kann, ohne `/proc/stat` zweimal zu lesen.

## Plattformverhalten

- **Linux:** `spg_machine_sample()` liefert `SPG_OK`. Einzelne fehlende Dateien
  sind kein Fehler — das jeweilige Feld bleibt `unknown`.
- **Alles andere** (macOS-Entwicklungsmaschine, BSD): `SPG_E_UNSUPPORTED`, alle
  Felder `unknown`, `timestamp_ns` gesetzt. Der Aufrufer kann weiterlaufen; ein
  Agent-Run darf daran nicht scheitern.

Kein `vcgencmd`. Das Throttling-Bit kommt primär aus dem hwmon-Gerät
`rpi_volt` (`in0_lcrit_alarm`), ersatzweise aus dem sysfs-Firmware-Interface.

Die Reihenfolge stammt aus einem Hardwaretest: auf einem Pi 5 mit Kernel 6.18
existiert `/sys/devices/platform/soc/soc:firmware/get_throttled` **nicht**, das
Feld wäre also auf genau der Zielhardware dauerhaft `unknown` geblieben.
`vcgencmd get_throttled` meldete dort `0xe0000` — Ereignisse seit Boot. Diese
Historie liefert nur `vcgencmd`, und das bleibt bewusst keine Abhängigkeit;
`past` ist deshalb nur dort erreichbar, wo der Firmware-Node existiert.

## Robustheit der Parser

Jeder Parser nimmt die Länge **vor** dem Puffer und liest höchstens `n` Bytes.
Keiner verlangt NUL-Terminierung; ein Test übergibt einen exakt passenden
Puffer, damit ASan jeden Überlauf fängt.

Der Parameter ist `buf[]`, nicht `buf[static n]`: eine leere Datei ist ein
legitimer Eingabefall, und `[static 0]` ist undefiniertes Verhalten. UBSan hat
das während der Entwicklung angezeigt — dieselbe Begründung wie beim leeren
Batch in `cmd_executor.h:88`.

Getestete Grenzfälle: fehlende Datei, leere Datei, Buchstaben im Zahlenfeld,
zu wenige Spalten in `/proc/stat`, 30-stellige Zahl (Overflow), Puffer ohne
NUL, `MemAvailable > MemTotal` (Clamp statt Wrap), Δt = 0, rückwärts laufende
Counter, Load ohne Dezimalpunkt, mehr als zwei Nachkommastellen (Trunkierung,
keine Rundung), Nicht-Hex im Throttle-Feld, Renderpuffer exakt ein Byte zu
klein.

## Auf Hardware verifiziert

Pi 5 Model B (aarch64, Kernel 6.18, Debian): `make test` grün, gemessener
Snapshot plausibel — CPU 6,2 %, Load 0,45, 4,2 GB Total / 1,68 GB Used,
Temperatur 47,95 °C, 1,5 GHz, 167 Prozesse.

Zwei Fehler fand erst dieser Lauf:
- `sysconf` war ohne `<unistd.h>` deklariert; macOS zieht den Header über
  `dirent.h` transitiv mit, glibc nicht.
- Der Throttling-Pfad existierte auf Pi 5 nicht (siehe oben).

## Was Phase 1 nicht tut

Keine Prozessliste (#62), keine Context-Integration (#63), kein History-Fenster
(#79), keine Action. Der Sampler wird von keinem Agent-Pfad aufgerufen — der
Journal-Freeze aus Phase 0 bleibt unverändert grün, und genau das belegt, dass
diese Phase die Runtime nicht angefasst hat.
