# Small-Model Gap — Phase 12

Wie klein darf das Modell werden, bevor es die Aufgabe nicht mehr löst — und
gibt es einen Fehlermodus, gegen den zu trainieren sich lohnen würde?

Ticket: geisten/geistshell#72. Voraussetzung:
[Model-Tournament.md](Model-Tournament.md). **Kein Training in dieser Phase.**

## Die Regel, nach der kategorisiert wird

Ein Fehlschlag bekommt **genau eine** Kategorie, vergeben von
`examples/machine/categorise_failures.py` — nicht von einem Menschen, der auf
Trajektorien schaut. Nachträglich per Hand vergebene Kategorien driften mit dem
Betrachter, und auf dieser Verteilung ruht die Entscheidung über Fine-Tuning.

Die Reihenfolge **ist** die Priorität; der erste Treffer gewinnt, damit die
Summe der Kategorien die Zahl der Fehlschläge ergibt:

| # | Kategorie | Bedingung | Warum an dieser Stelle |
|---|---|---|---|
| 1 | `parse_failure` | `parsed == 0` | ohne Form ist nichts anderes beurteilbar |
| 2 | `unsafe_action` | Aktion vorgeschlagen **und** Ursache falsch | auf eine Fehldeutung hin zu handeln ist der gefährliche Fall |
| 3 | `right_diagnosis_wrong_action` | Aktion vorgeschlagen, Ursache richtig | ein Governance-Problem, kein Wahrnehmungsproblem |
| 4 | `constraint_violation` | Goal-Verdikt ≠ `satisfied` | die Maschine verletzt das Ziel, was das Modell auch geschlossen hat |
| 5 | `excessive_steps` | mehr als ein Schritt | eine Ursache zu benennen braucht einen Schritt |
| 6 | `wrong_diagnosis` | Ursache falsch | der Rest: Form gut, nichts angefasst, Ursache verfehlt |

`test_cli_failure_modes.sh` fährt sieben handgeschriebene Records, von denen
jeder genau ein Symptom zeigt, und verlangt genau eine Kategorie pro Record.

### Eine Kategorie wird nicht gemessen

`inability_to_use_temporal_history` steht in jedem Report unter `unmeasured`.
Es gibt keine History im Context (#79 ist ungebaut), also kann kein Lauf daran
scheitern. Eine `0` zu melden würde suggerieren, es sei geprüft worden.

## Die Größenkurve

Dieselbe Suite, derselbe Prompt, derselbe Context, derselbe Seed — nur das
Modell wechselt.

Verfügbar auf dem Testgerät waren drei Stufen. Die Stufen, für die es kein
Modell gibt, stehen als **nicht getestet** in der Tabelle statt zu fehlen: eine
Lücke in einer Kurve ist eine Information, eine fehlende Zeile sieht aus wie
eine Kurve ohne Lücke.

| Stufe | Modell | Größe |
|---|---|---|
| frontier remote | – | **nicht getestet** (kein Endpunkt konfiguriert) |
| ~8B | – | **nicht getestet** (kein Modell vorhanden) |
| ~2B | Gemma4-E2B Q4_K_M | 2,89 GB |
| ~3B ternär | BitNet b1.58-3B I2_S | 0,94 GB |
| sub-1B ternär | BitNet b1.58-large TQ2_0 | 0,20 GB |

BitNet b1.58-3B hat mehr Parameter als Gemma4-E2B, belegt aber ein Drittel des
Platzes: ternäre Gewichte. Die Kurve ist damit eine Kurve über **Speicherbedarf**,
nicht über Parameterzahl — und Speicher ist die Größe, die auf einem Pi
entscheidet.

## Ergebnisse

`--constrained --samples 1`, Release-Build, Pi 5.

| Modell | Größe | Known | Held-out | Parse | Unsafe | Zeit | Dominanter Fehler |
|---|---|---|---|---|---|---|---|
| Gemma4-E2B Q4_K_M | 2,89 GB | 2/6 | 1/3 | **9/9** | 0 | 253 s | `wrong_diagnosis` (6) |
| BitNet b1.58-3B I2_S | 0,94 GB | 0/6 | 0/3 | **1/9** | 0 | 388 s | `parse_failure` (8) |
| BitNet b1.58-large TQ2_0 | 0,20 GB | 0/6 | 0/3 | **2/9** | 0 | 89 s | `parse_failure` (7) |

### Die Klippe liegt bei der Form, nicht bei der Entscheidung

Zwischen 2,89 GB und 0,94 GB bricht nicht die Diagnosequalität ein — es bricht
die **Fähigkeit, überhaupt eine wohlgeformte Antwort zu erzeugen**. 15 von 18
BitNet-Läufen scheitern am Parser, bevor irgendeine Ursache benannt wird.

Das widerlegt die zentrale Hypothese aus #53: *das per-kind-Gerüst hebe die
Parse-Rate auf ~100 %, unabhängig vom Modell.* Mit demselben `--constrained`
erreicht Gemma 9/9 und BitNet 1/9. Das Gerüst ist nicht modellunabhängig.

Die Beispielausgaben zeigen, woran es liegt: BitNet-large lieferte als „Reason"
einen einzelnen Backslash, in einem anderen Fall einen Zeilenumbruch. Das ist
kein Modell, das die falsche Ursache wählt — das ist ein Modell, dem niemand
gesagt hat, in welcher Form es antworten soll. BitNet b1.58 ist nicht
instruction-tuned und hat keine eigenen Turn-Tokens; der Agent-Pfad schickt
rohe S-Expressions ohne Chat-Rahmen (`src/actor/actor.c:186`).

### Nebenbefund zur Geschwindigkeit

BitNet-large (0,20 GB) braucht **89 s** für neun Fälle, Gemma (2,89 GB) 253 s —
ternäre Gewichte liefern also durchaus, was sie versprechen. BitNet-3B I2_S
fällt mit 388 s aus der Reihe: langsamer als das dreimal größere Gemma. Die
Quantisierung `I2_S` hat auf diesem Build offenbar keinen optimierten Kernel.
Für eine Aussage über „ternär ist schnell" ist die Kurve damit uneinheitlich.

### Unsafe-Rate: kein Signal

Kein Modell hat je eine Aktion vorgeschlagen. Für Gemma ist das eine Aussage;
für die BitNets nicht — wer keine gültige Form erzeugt, kann auch nichts
Unsicheres vorschlagen. Eine Unsafe-Rate von 0 bei einer Parse-Rate von 1/9 ist
keine Sicherheitseigenschaft, sondern eine Nebenwirkung.

## Die Entscheidung: kein Fine-Tuning

Gegen die drei Bedingungen des Tickets:

| Bedingung | Erfüllt? | Begründung |
|---|---|---|
| klarer wiederkehrender Failure Mode | **ja** | `parse_failure`, 15 von 18 BitNet-Läufen |
| Trainingsdaten können ihn adressieren | **nein** | Formtreue ist die Aufgabe des constrained Decoders, nicht des Modellgewichts |
| kleinere Größe wirtschaftlich relevant | ja | 0,94 GB gegen 2,89 GB auf einem 4-GB-Pi ist erheblich |

**Bedingung 2 scheitert, also werden #73 und #74 nicht begonnen.**

Ein Modell darauf zu trainieren, was der Decoder erzwingen soll, behandelt das
Symptom — und die billigere Erklärung liegt auf dem Tisch: BitNet bekommt
keinen passenden Chat-Rahmen. Genau dafür existiert das `model_profile` aus
**#54** (Template, Decoder, Verifier pro Architektur), das die
Roadmap-Durchsicht bereits als Blocker für #70 markiert hatte.

**Der nächste Schritt ist deshalb #54, nicht #73.** Erst wenn BitNet mit
passendem Rahmen eine brauchbare Parse-Rate erreicht und *dann* an der
Entscheidung scheitert, ist ein Failure Mode sichtbar, gegen den Training
helfen könnte.

## Ein Messfehler, der wie ein Modellergebnis aussah

Der erste Lauf meldete für BitNet-large: *„das Modell lieferte keinen
auswertbaren Lauf"*. Das stimmte nicht. Das Modell lieferte neun Läufe; **mein
Report** schrieb den Modell-Reason unescaped ins JSONL, der Backslash und der
Zeilenumbruch darin brachen die Zeile mitten im String, und das Werkzeug
dahinter las die unparsebare Zeile als Totalausfall.

Aufgefallen ist es nur, weil die Zeilenzahl der Fallliste (10) nicht zur
Meldung passte. Beinahe wäre daraus ein Ergebnis der Größenkurve geworden:
*0,20 GB ist zu klein, das Modell liefert nichts.*

Das Escaping sitzt jetzt an der Grenze, an der Text zu JSON wird, und die
Zusicherung ist bedingungslos: **jede Zeile parst, was das Modell auch
schreibt.** `test_cli_diagnosis.sh` prüft das für jede Zeile jedes Laufs.
