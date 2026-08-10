# Context Ablation — Phase 11

Wie groß muss die Welt des Agenten wirklich sein?

Ticket: geisten/geistshell#71. Voraussetzungen:
[Context.md](Context.md), [Model-Tournament.md](Model-Tournament.md).

## Eine Maske, keine sechs Renderer

Ablation entfernt Felder aus dem gerenderten Block über eine Bitmaske
(`SPG_ABLATE_ROLE`, `_TEMPERATURE`, `_FREQUENCY`, `_MEMORY`, `_PROCESSES`,
`_LOAD`). `spg_machine_state_render()` ist derselbe Aufruf mit Maske 0 — eine
Implementierung, damit ein ablatierter Block nicht vom vollen abdriften kann.

Ein ablatiertes Feld wird **weggelassen**, nicht auf `unknown` gesetzt. `unknown`
würde messen, wie das Modell mit einem defekten Sensor umgeht; das ist eine
andere Frage.

## Die Falle, gegen die getestet wird

Eine Variante, die auch ein Feld verändert, das sie nicht nennt, macht jede
Zahl in der Tabelle zu einer Aussage über zwei Änderungen gleichzeitig — und
keine Schlussfolgerung daraus hielte.

`test_machine_fixture.c` prüft deshalb pro Variante:

- das genannte Feld ist weg,
- ein Nachbarfeld ist **unverändert** da,
- der Block ist kleiner als der volle (eine Ablation, die nichts verkleinert,
  hat nichts getan),
- der Block ist weiterhin parsebar (sonst ist es ein kaputtes Experiment und
  kein kleinerer Context),
- Maske 0 ist byte-identisch zum ungemaskten Renderer,
- die Reihenfolge der Bits ändert nichts — eine Maske ist eine Menge.

## Die Varianten

| Variante | Maske | Frage |
|---|---|---|
| `full` | – | die Kontrolle |
| `no_role` | `role` | braucht das Modell die semantische Rolle, oder reichen die Zahlen? |
| `no_temperature` | `temperature` | Temperatur **und** Throttling — dasselbe Signal von zwei Seiten |
| `no_frequency` | `frequency` | vermutlich das erste entbehrliche Feld |
| `no_memory` | `memory` | kostet am meisten Bytes nach den Prozessen |
| `no_processes` | `processes` | die Prozessliste ist der teuerste Block |
| `minimal` | `frequency,memory,load` | nur Temperatur, Throttling und Prozesse |
| `bare` | alles | **der Boden** |

`bare` ist die wichtigste Zeile. Schneidet ein Modell dort gut ab, waren die
Szenarien zu leicht und jede andere Zahl der Tabelle ist bedeutungslos. Der
Block schrumpft dabei auf 35 Bytes — im Wesentlichen `(machine-state
(process-count N))`.

## Alles außer der Maske bleibt gleich

Dieselbe Suite, dasselbe Modell, dieselben Sampling-Parameter, derselbe Seed.
Der Runner ändert **nur** die Maske; eine Variante, die nebenbei die
Sampling-Parameter verstellt, misst zwei Dinge.

Die Context-Größe wird mit **derselben Maske** gemessen, die das Modell gesehen
hat. Die erste Fassung maß den vollen Block für jede Variante — die Tabelle
hätte für alle Zeilen dieselbe Größe gemeldet und das Experiment wäre nicht
falsifizierbar gewesen.

## Ergebnisse

### Scripted Control

| Variante | Known | Held-out | Context |
|---|---|---|---|
| full | 6/6 | 3/3 | 352 B |
| no_role | 6/6 | 3/3 | 327 B |
| no_temperature | 6/6 | 3/3 | 312 B |
| no_frequency | 6/6 | 3/3 | 329 B |
| no_memory | 6/6 | 3/3 | 267 B |
| no_processes | 6/6 | 3/3 | 219 B |
| minimal | 6/6 | 3/3 | 208 B |
| bare | 6/6 | 3/3 | 35 B |

Überall 6/6 — und das ist die richtige Antwort für diese Zeile: der scripted
Fake liest den Context nicht. Er zeigt, dass der Harness die Varianten korrekt
fährt und die Größen korrekt misst, mehr nicht. **Eine Ablationstabelle ohne
echtes Modell sagt nichts über Ablation aus.**

### Gemma4-E2B auf dem Pi 5

`--constrained --samples 1`, dieselbe Suite, derselbe Seed, nur die Maske
variiert.

| Variante | Known | Held-out | Unsafe | Context | Latenz |
|---|---|---|---|---|---|
| `full` | 2/6 | 1/3 | 0 | 352 B | 254 s |
| `no_role` | 2/6 | 1/3 | 0 | 327 B | 252 s |
| `no_temperature` | 2/6 | 1/3 | 0 | 312 B | 251 s |
| `no_frequency` | 3/6 | 1/3 | 0 | 329 B | 257 s |
| `no_memory` | 2/6 | 0/3 | 0 | 267 B | 252 s |
| `no_processes` | **1/6** | **0/3** | 0 | 219 B | 250 s |
| `minimal` | 2/6 | 1/3 | 0 | **208 B** | 237 s |
| `bare` | 1/6 | 0/3 | 0 | 35 B | 211 s |

**Die Antwort auf die Leitfrage, so weit sie hier zu haben ist:** `minimal` —
Temperatur, Throttling, Prozesse mit Rollen, ohne Frequenz, Speicher und
Systemlast — erreicht dieselben Werte wie der volle Context bei **59 % der
Bytes**. Für dieses Modell tragen Frequenz, Speicher und Systemlast keine
Information, die es benutzt.

**Das einzige Feld, dessen Verlust eindeutig schadet, ist die Prozessliste**
(2/6 → 1/6, 1/3 → 0/3). Das passt zur Konstruktion der Suite: vier der neun
Szenarien unterscheiden sich nur darin, **wer** die Ressource verbraucht.

### Was diese Tabelle nicht sagt

**`no_frequency` ist nicht besser.** 3/6 gegen 2/6 ist **ein** Fall bei
`--samples 1`. Ein einzelner Fall Unterschied ist bei einem Sample kein Signal,
sondern Rauschen. Ihn als Verbesserung zu lesen wäre genau das Cherry-Picking,
das #70 im Code ausschließt — hier muss die Disziplin im Text stehen, weil die
Zahl schon gedruckt ist.

**Der Boden hält.** `bare` (35 Bytes, praktisch nur `(process-count N)`) landet
bei 1/6 und 0/3 — auf dem Niveau einer konstanten Antwort. Die Szenarien sind
also **nicht** trivial zu raten, und der Rest der Tabelle bedeutet etwas.

**Und der ganze Context ist für dieses Modell wenig wert.** Zwischen `bare`
(1/6, 0/3) und `full` (2/6, 1/3) liegen ein bekannter und ein held-out Fall.
Das ist der ehrliche Informationsgehalt des Contexts für Gemma4-E2B — und der
Grund, die Leitfrage nur eingeschränkt zu beantworten: **ein Modell, das mit
allen Informationen 2/6 erreicht, kann kaum zeigen, welche davon es braucht.**
Gemessen wird hier zu einem guten Teil die Robustheit eines Rateverhaltens.

**Latenz hängt kaum am Context.** 211 s bis 257 s über eine 10-fache
Größenspanne: die Zeit geht ins Laden und Dekodieren, nicht in den Prompt. Für
ein kleines Modell auf einem Pi ist Context-Größe damit ein Qualitäts-, kein
Geschwindigkeitsargument.

**Aktionssicherheit ist unabhängig von der Ablation.** Kein Vorschlag einer
Aktion in irgendeiner Variante — auch nicht bei `bare`, wo das Modell fast
nichts weiß. Ein kleinerer Context macht dieses Modell nicht übergriffiger.

## Was fehlt

**History wurde nicht ablatiert.** Das Ticket nennt sie; es gibt sie nicht —
das Fenster ist #79 und ungebaut. Eine Variante, die ein nicht existierendes
Feld entfernt, hätte eine Zeile mit derselben Zahl wie `full` produziert und
den Anschein erweckt, History sei entbehrlich. Nicht gemessen ist ehrlicher.

**Policy-Hinweise und natürlichsprachliche Beschreibungen** sind ebenfalls
nicht ablatiert: sie stehen nicht im Machine-Block, sondern im Contract- und
Directive-Teil des Contexts, der aus anderen Phasen stammt. Sie zu maskieren
wäre ein Eingriff in den allgemeinen Context-Renderer und gehört nicht in eine
Phase über Maschinenzustand.

**Tokens statt Bytes:** gemessen werden Bytes. Eine Tokenzahl bräuchte den
Tokenizer des jeweiligen Modells im Harness — für einen Vergleich zwischen
Modellen wäre das nötig, für einen zwischen Varianten desselben Modells sind
Bytes ein monotoner Stellvertreter.
