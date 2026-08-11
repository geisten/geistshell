# Portability

Welche Maschine geistshell lesen kann — und wie eine neue dazukommt.

## Die Portgrenze

`include/geistshell/machine_backend.h` ist die **gesamte** Oberfläche, die
geistshell von einem Betriebssystem braucht. Alles darüber ist portables C:
Parser, Auswahl, Renderer, Policy, Ledger, Ziele. Alles darunter ist **eine
Datei pro Plattform**, ausgewählt beim Linken.

```
include/geistshell/machine_backend.h   der Vertrag
src/machine/backend_linux.c            /proc, /sys, hwmon
src/machine/backend_macos.c            Mach, sysctl, libproc
src/machine/backend_generic.c          alles unbekannt, ehrlich
```

Keine Funktionszeiger, kein Makro-Dispatch, kein `#if defined(__linux__)`
mitten in Logik, die mit Betriebssystemen nichts zu tun hat. Das Makefile
wählt über `uname -s`:

```make
ifeq ($(HOST_OS),Linux)
    MACHINE_BACKEND := src/machine/backend_linux.c
else ifeq ($(HOST_OS),Darwin)
    MACHINE_BACKEND := src/machine/backend_macos.c
else
    MACHINE_BACKEND := src/machine/backend_generic.c
endif
```

**Warum diese Grenze existiert:** die Lüfterklemmung aus Phase 15 stand einmal
im `#if defined(__linux__)`-Zweig. Eine Sicherheitseigenschaft, die auf keiner
anderen Plattform existierte und auf der Entwicklungsmaschine nicht getestet
werden konnte. Eine Portgrenze ist genau deshalb wertvoll: sie verhindert, dass
sich **Entscheidungen** in **I/O** verstecken.

## Was jede Plattform liefert

| Kanal | Linux | macOS | generic |
|---|---|---|---|
| CPU-Auslastung | `/proc/stat` | `host_statistics` | – |
| Speicher | `/proc/meminfo` | `hw.memsize` + `HOST_VM_INFO64` | – |
| Swap | `/proc/meminfo` | `vm.swapusage` | – |
| Load Average | `/proc/loadavg` | `getloadavg` | – |
| Temperatur | `thermal_zone0` | **nicht verfügbar** | – |
| CPU-Frequenz | `scaling_cur_freq` | `hw.cpufrequency` (nur Intel) | – |
| Throttling | hwmon `rpi_volt` | **nicht verfügbar** | – |
| Prozesse | `/proc/<pid>/stat` | `KERN_PROC_ALL` + `proc_pidinfo` | – |
| Prozess-Identität | Startzeit in Ticks | `p_starttime` in µs | – |
| Lüfter | hwmon `pwmfan` | **nicht verfügbar** | – |

**Was fehlt, wird gesagt statt gefälscht.** macOS hat keine öffentliche
Temperatur- oder Lüfterschnittstelle; die SMC-Schlüssel, die überall
kursieren, sind privat und ändern sich zwischen Modellen. Für eine Runtime,
die auditierbar sein soll, ist eine private API ein schlechter Tausch gegen
eine Zahl, die das Schema ohnehin als `unknown` zulässt.

## Zwei Statusarten, ein Unterschied

`spg_backend_is_live()` unterscheidet **„dieser Sensor fehlt"** von **„diese
Plattform wurde nie portiert"**. Ein Bericht, der beides gleich behandelt, wird
irgendwann falsch gelesen: 20 % Temperaturabdeckung heißt etwas anderes, wenn
die Hälfte der Hosts gar keinen Port hat.

## Der Vertrag, der beim Portieren zuerst bricht

**Aggregierte CPU-Zeit und `spg_process_sample.cpu_time` müssen dieselbe
Einheit haben.** Prozessauslastung ist das Verhältnis der beiden.

Der macOS-Port hat genau das im ersten Lauf verletzt: aggregiert in Mach-Ticks,
pro Prozess in Mikrosekunden. Ergebnis war nicht ein kleiner Fehler, sondern
**jeder Prozess bei 100 %** — eine Zahl, die plausibel genug aussieht, um durch
ein Review zu kommen. Deshalb steht die Regel jetzt im Header und nicht in
jemandes Kopf.

## CPU-Architekturen

Nichts in diesem Code ist architekturspezifisch: kein SIMD, keine Annahme über
Wortbreite, Endianness oder Seitengröße. Tickraten, Seitengrößen und
Zähler-Einheiten bleiben im Backend, das sie kennt; nach oben gehen nur
Verhältnisse und Bytes.

Verifiziert auf **aarch64** (Pi 5, Linux) und **arm64** (Apple Silicon, macOS).
x86-64 sollte ohne Änderung laufen, ist aber **nicht getestet** — und
ungetestet heißt hier ungetestet.

## Eine neue Plattform hinzufügen

1. `src/machine/backend_<os>.c` schreiben, alle Funktionen aus
   `machine_backend.h` implementieren.
2. Fehlendes gibt `SPG_E_UNSUPPORTED` **und** setzt den Ausgabewert auf
   `unknown` — nie auf 0.
3. Einheitenvertrag für CPU-Zeit einhalten (siehe oben).
4. Im Makefile eine `uname`-Zeile ergänzen.

Kein Test muss angefasst werden. Die Suite fragt `spg_backend_is_live()`, nicht
den Präprozessor — vorher stand in drei Tests `#if defined(__linux__)`, und
jeder davon hätte beim nächsten Port erneut editiert werden müssen.

## Was der macOS-Port an Testfehlern aufgedeckt hat

Zwei Prüfungen waren nur auf Linux-Daten korrekt:

- `test_cli_machine.sh` zählte Klammern, um die Wohlgeformtheit des Blocks zu
  prüfen. Auf Linux enthalten Prozessnamen selten Klammern; macOS liefert
  `Claude Helper (`, und der Zähler schlug an. Klammern **innerhalb einer
  Zeichenkette** sind Inhalt — die Prüfung ist jetzt String-bewusst.
- `test_cli_machine_run.sh` schaltete auf `uname -s = Linux` und las
  `/proc/<pid>/stat` für den Prozesszustand. Beides ersetzt durch
  `ps -o stat=`, das beide Kernel beantworten.

Beide Fehler prüften seit Monaten weniger, als sie behaupteten. Ein zweiter
Port ist das billigste Werkzeug, um so etwas zu finden.
