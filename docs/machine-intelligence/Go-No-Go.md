# Go / No-Go — Phase 17

Nüchterne Auswertung aller Benchmarks. Keine neuen Features.

Ticket: geisten/geistshell#77. Jede Antwort verweist auf eine gemessene Zahl;
wo keine existiert, steht das statt einer Schätzung.

## Die zwölf Fragen

### 1. Schlägt AI die Rule Baseline?

**Nein. Nicht annähernd.**

| Methode | Known | Held-out | Halluzination | Unsafe | Zeit |
|---|---|---|---|---|---|
| **Regeln** | **6/6** | **3/3** | 0 | 0 | < 1 ms |
| Gemma4-E2B Q4_K_M (2,89 GB) | 2/6 | 1/3 | 0 | 0 | 253 s |
| BitNet b1.58-3B (0,94 GB) | 0/6 | 0/3 | 0 | 0 | 388 s |
| BitNet b1.58-large (0,20 GB) | 0/6 | 0/3 | 0 | 0 | 89 s |

Quelle: [Diagnosis-Benchmark.md](Diagnosis-Benchmark.md),
[Small-Model-Gap.md](Small-Model-Gap.md).

### 2. Bei welchen Szenarien?

Bei **allen neun**. Das beste Modell löst drei, und einer davon
(`critical_cpu`) ist schwer von Glück zu unterscheiden: dieselbe Antwort war im
`healthy`-Szenario falsch.

### 3. Wie groß ist der Gewinn?

Negativ: **6 von 9 Fällen zugunsten der Regeln**, bei rund fünf Größenordnungen
weniger Rechenzeit.

**Aber die Regeln sind nicht robust.** Der Sensitivitätslauf über die
Schwellwerte:

| Schwellwerte | Treffer |
|---|---|
| Default | 9/9 |
| alle −10 % | 9/9 |
| alle **+10 %** | **6/9** |

Nach oben verschoben brechen drei Fälle — vor allem die Speicherfälle, die bei
95 % und 96,6 % Auslastung dicht an der 90-%-Schwelle liegen. Die 9/9 sind
echt, aber sie stehen auf Zahlen, die nicht weit von den Daten entfernt sind.

**Und der schwerste Vorbehalt:** Regeln und Szenarien haben denselben Autor.
Die Szenarien entstanden in Phase 4 vor den Regeln in Phase 5, was hilft, aber
es ersetzt keine unabhängige Testmenge.

### 4. Wie oft schlägt das Modell unsichere Aktionen vor?

**Nie** — in keinem Modell, keiner Ablationsvariante, keinem Lauf.

Für Gemma ist das eine Aussage. Für die BitNets nicht: wer keine wohlgeformte
Antwort erzeugt (1/9 bzw. 2/9 Parse), kann auch nichts Unsicheres vorschlagen.
Eine Unsafe-Rate von 0 bei einer Parse-Rate von 1/9 ist eine Nebenwirkung.

### 5. Wie oft verhindert die Policy erfolgreich Schaden?

In **jedem** dafür konstruierten Fall — aber die Fälle waren scripted, nicht
von einem Modell erzeugt.

Neun von zwölf Red-Team-Angriffen werden durch eine Schicht gestoppt, mit der
ein Modell nicht diskutieren kann. Siehe [Security-Review.md](Security-Review.md).
Der Bypass-Test ist der aussagekräftigste: ein Modell greift nach `local_shell`,
und die Konfiguration vergibt diese Capability gar nicht.

**Die Policy hat noch nie echten Schaden verhindert**, weil kein Modell je
Schaden versucht hat. Gemessen ist die Sperre, nicht ihre Notwendigkeit.

### 6. Welches kleinste Modell liegt innerhalb von 3 Prozentpunkten des besten?

**Keines.** Das beste Modell erreicht 3/9; das zweitbeste 0/9. Die Frage setzt
eine Qualitätskurve voraus, die es nicht gibt — es gibt eine Klippe.

### 7. RAM-, Latency- und CPU-Bedarf?

Für das beste Modell (Gemma4-E2B): 2,89 GB Datei auf einem 4-GB-Pi, 253 s für
neun Fälle. **Peak RSS und CPU während der Inferenz sind nicht instrumentiert**
— der Harness misst Wanduhr, nicht Speicher.

### 8. Welche Context-Informationen sind wirklich nötig?

`minimal` — Temperatur, Throttling, Prozesse mit Rollen — erreicht dieselben
Werte wie der volle Context bei **59 % der Bytes**. Die einzige Ablation, die
eindeutig schadet, ist die Prozessliste.

Einschränkung aus [Context-Ablation.md](Context-Ablation.md): zwischen leerem
und vollem Context liegen für Gemma ein bekannter und ein held-out Fall. Das
ist der gesamte Informationswert des Contexts für dieses Modell.

### 9. Generalisiert es auf Machine B?

**Die Governance ja, der Action Space nein.**

Policy Gate, Budget, Journal, Replay und Eval trugen den Lüfter **ohne eine
einzige Änderung**. Siehe [Machine-B.md](Machine-B.md).

### 10. Wie viel maschinenspezifischer Code war nötig?

208 Zeilen Adapter, **37 Zeilen in sechs Core-Dateien**, **null Zeilen reine
Konfiguration** — für *eine* Aktion.

### 11. Ist der Agent-Layer generisch oder nur für den Pi passend?

**Generisch in der Governance, Pi-spezifisch in der Telemetrie.**

Die Telemetrie ist Linux-only und liest `/proc` und `/sys`; macOS degradiert
sauber zu `unknown`. Die Governance-Schicht enthält nichts Maschinenspezifisches.

Der geschlossene Action-Enum liegt dazwischen: er ist nicht Pi-spezifisch, aber
jede neue Maschine kostet Änderungen an sechs Stellen.

### 12. Welche Probleme löst klassische Software besser?

**Diese.** Vier Schwellwertvergleiche über eine feste Feldliste lösen die Suite
vollständig in unter einer Millisekunde. Ein 2,89-GB-Modell braucht 253 s für
ein Drittel davon.

Genauer: klassische Software gewinnt überall dort, wo die Zustandsmenge klein,
die Signale numerisch und die Ursachenmenge geschlossen ist — also genau in dem
Regime, das die Kernhypothese als „kleine, strukturierte, kontrollierbare Welt"
beschreibt. **Je besser die Welt zur Hypothese passt, desto weniger braucht sie
ein Modell.**

## Die drei Forschungsmetriken

| Metrik | Ergebnis |
|---|---|
| **Quality vs. Model Size** | Keine Kurve, eine Klippe. Zwischen 2,89 GB und 0,94 GB bricht die **Form** zusammen (9/9 → 1/9 Parse), nicht die Qualität. |
| **Safety under Model Failure** | Die Sperre hält in jedem konstruierten Fall; ihre Notwendigkeit ist ungemessen, weil kein Modell je etwas Unsicheres vorschlug. |
| **Machine Integration Effort** | 6 Core-Dateien, 37 Zeilen, 0 Konfigurationszeilen pro Aktion. Governance: 0 Änderungen. |

## Empfehlung: **PIVOT**

> geistshell als Governance- und Evaluations-Runtime, aber kein eigenes kleines
> Modell.

Begründet ausschließlich mit den Messungen oben:

**Gegen ein eigenes Modell** sprechen Frage 1, 3, 6 und 12. Die Regel-Baseline
schlägt jedes getestete Modell in jedem Szenario bei fünf Größenordnungen
weniger Rechenzeit. Es gibt kein kleinstes Modell innerhalb von 3 Prozentpunkten
des besten, weil es keine Kurve gibt. Und der dominante Fehler der kleinen
Modelle ist Formtreue — die Aufgabe des Decoders, nicht des Gewichts (#72).

**Für die Runtime** sprechen Frage 5, 9 und 11. Die Governance trug eine zweite
Maschine ohne Änderung. Der Bypass-Versuch scheiterte an der Konfiguration, nicht
an einer Prompt-Formulierung. Der Journal-Freeze aus Phase 0 hielt über sechzehn
Phasen und über zwei Architekturen byte-identisch.

**Kein STOP**, weil die Governance-Schicht messbar funktioniert und das Ergebnis
selbst — dass Regeln hier gewinnen — nur zu haben war, weil eine Eval-Runtime
existierte, die es zeigen konnte.

**Kein GO**, weil ein OEM Machine Agent Runtime ein Modell voraussetzt, das
seinen Platz verdient, und keines hat ihn verdient.

## Was diese Empfehlung nicht trägt

- **Kein Remote-Frontier-Modell getestet.** Kein Endpunkt konfiguriert. Die
  Aussage „AI schlägt die Regeln nicht" gilt für zwei lokale Architekturen auf
  einem Pi, nicht für die Klasse.
- **Ein Sample pro Fall.** Unterschiede von ein bis drei Fällen sind Rauschen.
- **Neun Szenarien, ein Autor.** Regeln und Testfälle stammen von derselben
  Hand; die Regeln überstehen −10 %, aber nicht +10 %.
- **Keine echte Prozesslast.** Die Diagnose-Szenarien sind Fixtures. Die
  Workloads aus #68 sind gebaut, aber nie gegen ein Modell gefahren.

Der ehrlichste Satz zum Schluss: **das wertvollste Ergebnis dieser Roadmap ist
ein negatives**, und es war nur zu haben, weil die Infrastruktur existierte, um
es zu messen.
