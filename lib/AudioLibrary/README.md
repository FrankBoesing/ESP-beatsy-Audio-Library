# ESP-beatsy Audio Library – technische Übersicht

Diese Datei dokumentiert die Task-Architektur, das Timing, die wesentlichen Puffer sowie ausgewählte DSP- und FFT-Komponenten der ESP-beatsy Audio Library. Die Bibliothek orientiert sich an der Teensy Audio Library und verwendet in der `AudioStream`-Schnittstelle weiterhin **16-Bit-PCM**.

> **Hinweis zum Stand:** Task-Stacks, Prioritäten, Queue-Größen und Pufferkapazitäten sind Implementierungsdetails und können sich zwischen Branches oder Revisionen ändern. Die konkreten Zahlen unten beschreiben den Arbeitsstand, aus dem diese Dokumentation erstellt wurde. Bei Änderungen am Quellcode sollten insbesondere diese Werte erneut überprüft werden.

## Inhaltsverzeichnis

- [1. Architektur und Tasks](#1-architektur-und-tasks)
- [2. Timing](#2-timing)
- [3. Puffer und Speicherbedarf](#3-puffer-und-speicherbedarf)
- [4. Datenpfad beim Netzwerk-Streaming](#4-datenpfad-beim-netzwerk-streaming)
- [5. DSP-Komponenten](#5-dsp-komponenten)
- [6. FFT-Analyzer: FFT256 und FFT1024](#6-fft-analyzer-fft256-und-fft1024)
- [7. Dynamische Lebensdauer von AudioStream-Objekten](#7-dynamische-lebensdauer-von-audiostream-objekten)
- [8. Hinweise für die Praxis](#8-hinweise-für-die-praxis)

---

## 1. Architektur und Tasks

Die Bibliothek trennt die Echtzeitverarbeitung des Audio-Graphs von Decoder- und Netzwerk-I/O. Mixer, Filter und andere `AudioStream`-Objekte werden durch `AudioTask` verarbeitet. Bei aktiver I2S-Ausgabe übernimmt `AudioI2STx` die Übergabe der Stereo-Daten an den I2S-Treiber.

```text
HTTP-Stream
    |
    v
AudioStreamRx --AudioSourceStream-Ringpuffer--> AudioDecoder
                                                  |
                                                  | PCM-Puffer
                                                  v
Audio-Graph / AudioTask --Stereo-Queue--> AudioI2STx --> I2S
       ^
       |
Software-Timer oder I2S-TX-DMA-Callback
```

Die Netzwerk- und Decoder-Pfade sind nur aktiv, wenn die entsprechende Quelle beziehungsweise Wiedergabe verwendet wird. Die I2S-Ausgabe-Task wird nur bei aktivierter I2S-Ausgabe gestartet.

### Tasks im Überblick

Die Stack-Größen beziehen sich auf die in dieser Arbeitsversion verwendeten Werte. Prioritäten werden relativ zu `configMAX_PRIORITIES` angegeben, damit sie nicht von einer bestimmten FreeRTOS-Konfiguration abhängen.

| Task | Erstellt durch | Priorität | Stack-Größe | Core | Aufgabe |
| --- | --- | --- | ---: | --- | --- |
| `loopTask` | Arduino-Framework | `1` | standardmäßig 8192 Byte, konfigurierbar | `ARDUINO_RUNNING_CORE` | Führt `setup()` und anschließend wiederholt `loop()` aus. Beispielprogramme warten hier etwa auf den Stream-Prebuffer und geben Diagnosedaten aus. |
| `AudioTask` | `AudioStream::update_setup()` | `configMAX_PRIORITIES - 2` | 4096 Byte | `AUDIO_PROCESSING_CORE` (Standard: Core 1) | Verarbeitet aktive `AudioStream`-Objekte, darunter Mixer und Effekte. Wartet auf Benachrichtigungen und führt selbst kein blockierendes Audio-I/O aus. |
| `AudioDecoder` | `AudioDecoderStream::startDecoderTask()` | `configMAX_PRIORITIES - 3` | 8192 Byte | `AUDIO_DECODER_CORE` (Standard: Core 0) | Dekodiert MP3-/AAC-Daten und füllt die abwechselnd verwendeten PCM-Puffer im internen RAM. Wird bei entsprechender Wiedergabe gestartet. |
| `AudioI2STx` | `AudioOutputI2S::beginInternal()` | `configMAX_PRIORITIES - 2` | 4096 Byte | `AUDIO_PROCESSING_CORE` (Standard: Core 1) | Entnimmt Stereo-Blöcke aus der Queue, konvertiert sie in I2S-Daten und übergibt sie an den I2S-Treiber. |
| `AudioStreamRx` | `AudioSourceStream::open()` | `configMAX_PRIORITIES - 5` | 8192 Byte | nicht fest zugeordnet | Liest Netzwerkdaten aus dem HTTP-Stream in einen temporären Chunk und kopiert diese in den Ringpuffer im PSRAM. Wird nur für Netzwerk-Streams benötigt. |

Zusätzlich verwaltet ESP-IDF unter anderem Wi-Fi-, TCP/IP-, Event-Loop-, Timer-Service- und Idle-Tasks. Deren Prioritäten und Core-Zuordnungen hängen von der SDK-Konfiguration ab.

Der DMA-Callback der I2S-Ausgabe ist eine **ISR**, keine FreeRTOS-Task. Er benachrichtigt bei aktivem I2S-Takt `AudioTask`, nachdem ein DMA-Block verarbeitet beziehungsweise übertragen wurde. Ohne I2S-Ausgabe kann ein periodischer Software-Timer die Audio-Updates takten.

## 2. Timing

Die Standard-Blockgröße beträgt `AUDIO_BLOCK_SAMPLES == 128` Samples. Die Dauer eines Audio-Blocks ist:

\[
T_\text{Block} = \frac{\texttt{AUDIO\_BLOCK\_SAMPLES}}{f_s}
\]

Bei 128 Samples und 44.100 Hz ergibt sich:

\[
\frac{128}{44100}\,\text{s} \approx 2{,}90\,\text{ms}
\]

Wird die Sample-Rate oder die Blockgröße geändert, ändert sich die Blockperiode entsprechend.

- **Ohne I2S-Ausgabe:** Ein periodischer Software-Timer löst die Audio-Updates aus.
- **Mit I2S-Ausgabe:** Der I2S-TX-DMA-Callback liefert den Takt und benachrichtigt `AudioTask`; der Software-Audio-Timer ist dann abgeschaltet.
- **Timeouts:** Die in dieser Arbeitsversion verwendeten Timeouts von bis zu 100 ms für Queue-Warten beziehungsweise I2S-Schreiben sind Fehler- und Leerlaufgrenzen. Sie sind **nicht** die Audio-Blockperiode.

Für eine störungsfreie Wiedergabe müssen die Echtzeitverarbeitung und die Übergabe der Blöcke rechtzeitig erfolgen. Aufwendige DSP-Verarbeitung oder ein zu knapp dimensionierter Speicherpool kann zu Aussetzern führen.

## 3. Puffer und Speicherbedarf

Die folgende Tabelle unterscheidet den globalen Audio-Blockpool von separaten I2S-, Decoder- und Streaming-Puffern. Größen sind, soweit nicht anders angegeben, dezimale Byte-Angaben; die Kapazitäten gelten für den oben beschriebenen Arbeitsstand.

| Puffer / Speicher | Größe bzw. Kapazität | Zweck und Hinweise |
| --- | --- | --- |
| `AudioMemory(n)` / Audio-Blockpool | `n` Blöcke; standardmäßig 128 Mono-Samples pro Block | `AudioMemory(n)` legt den Pool statisch an; `n` wird vom Anwendungsprogramm festgelegt. Ein Block enthält bei 128 Samples 256 Byte PCM sowie 4 Byte Verwaltungsfelder, insgesamt 260 Byte für `audio_block_t`. Hinzu kommt die Verfügbarkeitsmaske des Pools. |
| I2S-Stereo-Queue | 4 Stereo-Blockpaare | Enthält Zeiger auf Blöcke, keine zusätzlichen PCM-Kopien. |
| I2S-Ausgabepuffer | 1024 Byte pro Block | Temporärer Puffer für 128 Stereo-Frames mit je zwei 32-Bit-Samples vor der Übergabe an den Treiber. |
| I2S-DMA | 16 Deskriptoren mit jeweils 128 Stereo-Frames | Bei zwei 32-Bit-Slots pro Frame ergibt das ungefähr 16 KiB DMA-Nutzdaten. |
| MP3-Eingabepuffer | 2 KiB pro `AudioPlayMp3`-Objekt | Enthält komprimierte Daten im internen RAM. Wird auch beim dateibasierten MP3-Pfad verwendet. |
| MP3-PCM-Puffer | 2 Puffer mit je 2304 `int16_t`-Samples | 4608 Byte pro Puffer, insgesamt 9216 Byte pro MP3-Objekt. Die Puffer werden abwechselnd verwendet. |
| `AudioInputBuffer` | standardmäßig 32 KiB | Eigenständige, generische Buffer-Klasse. Die Größe ist über den Konstruktor oder `AUDIO_INPUT_BUFFER_SIZE` konfigurierbar; auf ESP32 wird PSRAM bevorzugt, sofern verfügbar. Der beschriebene MP3-Wiedergabepfad verwendet diese Klasse derzeit nicht. |
| `AudioSourceStream` | 128 KiB | Separater Ringpuffer im PSRAM für Netzwerk-Streams. Er wird von `AudioStreamRx` gefüllt und ist unabhängig vom `AudioInputBuffer`. |

### Wichtige Konsequenz für `AudioMemory()`

`AudioMemory(n)` dimensioniert den Audio-Blockpool und nicht die separaten I2S-, Decoder- oder Stream-Ringpuffer. Die Zahl der benötigten Blöcke hängt vom gesamten Audio-Graphen und von den Effekten ab, die zusätzliche Blöcke anfordern oder länger halten.

Insbesondere hält `AudioAnalyzeFFT1024` seine Eingangsblöcke bis zur Verarbeitung eines vollständigen FFT-Frames fest. Bei 128 Samples pro Block sind das 8 Blöcke, bei 64 Samples pro Block 16 Blöcke. Diese Blöcke müssen zusätzlich zu den Anforderungen der vorgeschalteten Quellen, Effekte und übrigen Graph-Knoten verfügbar sein.

## 4. Datenpfad beim Netzwerk-Streaming

Bei der Netzwerk-Wiedergabe verläuft der Datenweg vereinfacht wie folgt:

1. `AudioStreamRx` liest verfügbare HTTP-Daten in einen temporären Chunk von 2 KiB.
2. Der Chunk wird in den 128-KiB-Ringpuffer von `AudioSourceStream` im PSRAM kopiert.
3. Der Decoder übernimmt verfügbare Daten aus diesem Ringpuffer in seinen 2-KiB-Eingabepuffer im internen RAM.
4. `MP3Decode()` verarbeitet die komprimierten Daten und schreibt dekodiertes PCM in die beiden wechselnden Decoder-Puffer.
5. Der Audio-Graph verarbeitet die PCM-Daten im Takt von `AudioTask`; anschließend gelangen Stereo-Blöcke über die I2S-Queue zu `AudioI2STx`.

**Datei-Wiedergabe:** Dateibasierte MP3-Wiedergabe umgeht den `AudioSourceStream`-Ringpuffer und verwendet weiterhin den 2-KiB-MP3-Eingabepuffer.

**`AudioInputBuffer`:** Diese Klasse ist ein eigenständiger Baustein und kein zusätzlicher Puffer des beschriebenen MP3-Pfads. Ihre Füllregeln – bei Dateien in 2-KiB-Schritten und bei Streams bis zu einem 32-KiB-Ziel – gelten nur, wenn `AudioInputBuffer::fill()` explizit verwendet wird. Diese Füllziele sind keine weiteren, parallel reservierten Puffer.

## 5. DSP-Komponenten

### `AudioSynthNoiseWhite`

Erzeugt weißes Rauschen. `amplitude()` erwartet einen Wert von `0.0` bis `1.0`. Die Implementierung verwendet einen 31-Bit-Park-Miller-Zufallszahlengenerator und skaliert die Samples mit 32-Bit-Arithmetik.

### `AudioSynthWaveformDc`

Erzeugt einen konstanten Gleichspannungspegel im Bereich von `-1.0` bis `1.0`.

- `amplitude(level)` setzt den Pegel ohne Übergangsrampe.
- `amplitude(level, milliseconds)` fährt mit der aktuellen Sample-Rate weich auf den neuen Pegel.
- `read()` liefert den aktuellen Pegel normiert auf `-1.0` bis `1.0`.

### `AudioEffectMultiply`

Multipliziert die beiden Eingangssignale sampleweise. Das Produkt wird in 32 Bit berechnet, mit Q15-Skalierung auf 16-Bit-PCM zurückgeführt und dabei begrenzt. Fehlt einer der beiden Eingangsblöcke, erzeugt der Effekt in diesem Update keinen Ausgangsblock.

### `AudioEffectGranular`

`begin(sampleBank, maxLength)` übergibt dem Effekt einen vom Aufrufer bereitgestellten Sample-Puffer. Der Effekt kopiert diesen Puffer nicht; er muss daher während der Nutzung gültig bleiben.

- `beginFreeze()` nimmt ein Grain an einem Nulldurchgang auf und wiederholt es.
- `beginPitchShift()` nimmt Grains auf und wiederholt sie mit weicher Ausblendung.
- `setSpeed()` begrenzt das Wiedergabeverhältnis auf `0.125` bis `8.0`.
- Für Pitch-Shift muss der Sample-Puffer mindestens dreimal so lang sein wie die gewünschte maximale Grain-Länge.
- `stop()` deaktiviert den Effekt; das Eingangssignal wird durchgereicht.

### `AudioFilterBiquad`

Bietet bis zu vier kaskadierte Filterstufen. Verfügbare Entwurfsfunktionen sind `setLowpass()`, `setHighpass()`, `setBandpass()`, `setNotch()`, `setLowShelf()` und `setHighShelf()`.

`setCoefficients(stage, int*)` erwartet fünf bereits in Q30 kodierte Koeffizienten in der Reihenfolge `b0, b1, b2, a1, a2`. Die `double*`-Variante erwartet normalisierte Fließkomma-Koeffizienten – insbesondere bereits auf `a0 = 1` normiert – und wandelt sie intern in Q30 um. Die Filterlaufzeit verwendet Q30-Koeffizienten und 64-Bit-Zustände beziehungsweise Akkumulatoren; die Ausgabe wird auf 16-Bit-PCM begrenzt.

### `AudioFilterFIR`

`begin(coefficients, count)` akzeptiert Teensy-kompatible Q15-Koeffizienten. Gültig sind gerade Koeffizientenzahlen von 4 bis 200. Die Koeffizienten werden in die interne Struktur kopiert; die Laufzeit akkumuliert Produkte in 64 Bit. Die Reihenfolge entspricht der zeitlich rückwärts gespeicherten CMSIS-Reihenfolge `bN-1` bis `b0`.

- `begin(FIR_PASSTHRU, 0)` aktiviert den ungefilterten Durchlauf; die Koeffizientenzahl wird für diesen Sonderwert nicht verwendet.
- `end()` deaktiviert den Filter.

FIR-Filter mit vielen Koeffizienten können einen erheblichen Anteil der verfügbaren CPU-Zeit beanspruchen.

## 6. FFT-Analyzer: FFT256 und FFT1024

Die Klassen `AudioAnalyzeFFT256` und `AudioAnalyzeFFT1024` stellen eine Teensy-ähnliche Schnittstelle mit `available()`, `read(bin)`, `read(first, last)` und `windowFunction()` bereit. Beide verwenden 50 % Frame-Überlappung und unterstützen `AUDIO_BLOCK_SAMPLES == 64` oder `128`.

### Vergleich

| Eigenschaft | `AudioAnalyzeFFT256` | `AudioAnalyzeFFT1024` |
| --- | ---: | ---: |
| FFT-Länge | 256 Samples | 1024 Samples |
| Verfügbare Bins | 0 bis 127 | 0 bis 511 |
| Frame-Dauer bei 44,1 kHz | ca. 5,80 ms | ca. 23,22 ms |
| Frequenzabstand `f_s / N` bei 44,1 kHz | ca. 172,27 Hz | ca. 43,07 Hz |
| Datenhaltung | Kopiert Samples in einen internen Frame-Puffer und gibt Eingangsblöcke anschließend frei | Hält die `audio_block_t`-Eingangsblöcke bis zur FFT fest; gibt nach der Verarbeitung die ältere Hälfte frei und behält die neuere Hälfte für den Overlap |
| Mittelung | Standardmäßig Mittelung über 8 Frames; über `averageTogether(n)` konfigurierbar | `averageTogether()` bleibt aus Kompatibilitätsgründen vorhanden, führt aber keine Mittelung aus |

Der nächste Analyse-Frame beginnt jeweils nach der halben FFT-Länge. Die Frequenz des Bins `k` ergibt sich näherungsweise aus:

\[
f_k = k \cdot \frac{f_s}{N}
\]

Dabei ist `f_s` die aktuelle Sample-Rate und `N` die FFT-Länge. Die Frequenzabstände in der Tabelle gelten daher für 44,1 kHz; bei einer anderen Sample-Rate ändern sie sich entsprechend.

### Fensterung, Ausgabe und Synchronisierung

- Die Standardfenster sind Hanning-Fenster (`AudioWindowHanning256` bzw. `AudioWindowHanning1024`).
- `windowFunction(w)` erlaubt die Auswahl eines anderen Q15-Fensters. Der Analyzer kopiert den übergebenen Koeffizientenpuffer nicht; ein benutzerdefiniertes Fenster muss deshalb während der Verwendung gültig bleiben. Mit `windowFunction(nullptr)` wird die Fensterung deaktiviert.
- `read(bin)` gibt für ungültige Bin-Indizes `0.0f` zurück. `read(first, last)` summiert die Magnituden des angegebenen Bin-Bereichs; es bildet keinen Mittelwert.
- Die Ausgabeskalierung ist auf die Teensy-Konvention ausgerichtet: `read(bin)` skaliert den gespeicherten Wert mit `1 / 16384.0f`.
- `AudioAnalyzeFFT256` mittelt standardmäßig das Betragsquadrat über acht Frames und zieht erst am Ende die Quadratwurzel. `averageTogether(0)` wird als eine Einzelmessung behandelt.

### FFT-Backend und ESP32-S3

Das gemeinsame Backend liegt in `utility/audio_fft_backend.h` und `utility/audio_fft_backend.cpp`.

1. Wenn ESP-DSP verfügbar und korrekt eingebunden ist, verwendet das Backend die komplexe Radix-4-FFT von ESP-DSP.
2. Der ESP32-S3-AES3-Kernel wird nur dann verwendet, wenn die ESP-DSP-Bibliothek mit dem passenden optimierten Kernel gebaut wurde und das entsprechende Build-Makro (`dsps_fft4r_fc32_aes3_enabled`) aktiv ist. Ein sichtbarer Header allein reicht nicht; die ESP-DSP-Komponente muss auch gelinkt sein.
3. Wenn ESP-DSP nicht verfügbar ist oder die benötigten Tabellen nicht initialisiert werden können, verwendet das Backend eine portable Float-Radix-2-FFT mit vorberechneten Twiddle-Faktoren. Dieser Fallback benötigt weder ARM noch CMSIS-DSP.

### Speicherverhalten von FFT1024

`AudioAnalyzeFFT1024` hält absichtlich die Eingangsblöcke bis zur Verarbeitung des vollständigen 1024-Sample-Frames. Bei 128 Samples pro Block werden 8 Blöcke gehalten, bei 64 Samples pro Block 16. Nach jeder Transformation werden die älteren 50 % freigegeben, während die neuere Hälfte für den nächsten überlappenden Frame erhalten bleibt.

Die Zahl der mit `AudioMemory()` bereitgestellten Blöcke muss deshalb die gehaltenen FFT-Blöcke **und** die Blöcke für vorgelagerte Quellen, Effekte und andere Graph-Knoten abdecken. Ist der Pool zu klein, können vorgeschaltete Objekte keine Blöcke mehr anfordern und der FFT-Frame wird nicht vollständig.

Die ältere Teensy-Implementierung von FFT256 hatte einen dokumentierten Fehlerfall mit `AUDIO_BLOCK_SAMPLES == 64`, bei dem `available()` nicht korrekt gesetzt wurde. Siehe [Teensy Audio Issue #379](https://github.com/PaulStoffregen/Audio/issues/379). Die hier beschriebene Implementierung verwendet eine gemeinsame Frame-Verwaltung mit 50 % Overlap anstelle der alten Sonderfall-Verwaltung.

### Hinweis zu `sqrt_integer`

Die angepasste `utility/sqrt_integer.h` lässt sowohl `sqrt_uint32(0)` als auch `sqrt_uint32_approx(0)` direkt mit `0` zurückkehren, bevor `__builtin_clz()` aufgerufen wird. Damit können diese Helfer auch von anderen Aufrufern mit einem Eingabewert von null verwendet werden, ohne dass jeder Aufrufer einen eigenen Sonderfall prüfen muss.

## 7. Dynamische Lebensdauer von `AudioStream`-Objekten

Standardmäßig ist `AUDIOSTREAM_ENABLE_DYNAMIC_LIFETIME` deaktiviert. Das entspricht dem üblichen Teensy-Anwendungsmodell: Audio-Objekte und ihre Verbindungen werden statisch angelegt und bleiben während der Audiowiedergabe bestehen.

### Statische Objekte – Standardfall

```cpp
AudioSynthWaveform synth;
AudioMixer4 mixer;
AudioOutputI2S output;

AudioConnection c1(synth, 0, mixer, 0);
AudioConnection c2(mixer, 0, output, 0);
```

Dieser Ansatz ist für die meisten Anwendungen vorzuziehen, weil die Lebensdauer des Audio-Graphs klar und dauerhaft ist.

### Dynamische Objekte

Wenn `AUDIOSTREAM_ENABLE_DYNAMIC_LIFETIME` aktiviert ist, entfernt der `AudioStream`-Destruktor das Objekt aus der Update-Liste und löst verbundene `AudioConnection`-Objekte. Das Makro muss als Build-Definition für die Bibliothek und den Sketch konsistent aktiviert werden; es sollte nicht nur lokal in einer einzelnen Quelldatei definiert sein. Füge dafür `-DAUDIOSTREAM_ENABLE_DYNAMIC_LIFETIME` zu den bestehenden `build_flags` der betreffenden PlatformIO-Umgebung hinzu.

```cpp
auto *synth = new AudioSynthWaveform;
auto *output = new AudioOutputI2S;
auto *connection = new AudioConnection(*synth, 0, *output, 0);

// ... Audio läuft ...

AudioStream::disableUpdates();
delete connection;  // Verbindung explizit zuerst trennen
connection = nullptr;
delete synth;
synth = nullptr;
delete output;
output = nullptr;
AudioStream::enableUpdates();
```

Die Reihenfolge ist bewusst gewählt: Zuerst wird die Verbindung gelöscht, dann werden Quelle und Ziel freigegeben. `disableUpdates()`/`enableUpdates()` schützen den zusammenhängenden Umbau vor einer gleichzeitigen Traversierung des Audio-Graphs. Die Aufrufe müssen paarig und in derselben Kontrollstrecke erfolgen.

Ohne `AUDIOSTREAM_ENABLE_DYNAMIC_LIFETIME` würden bei `delete` die Einträge der `AudioStream`-Objekte in der Update-Liste nicht automatisch entfernt. Der Scheduler könnte dann später auf freigegebenen Speicher zugreifen. Dynamische Audio-Objekte sollten daher nur verwendet werden, wenn die entsprechende Unterstützung für die gesamte Build-Konfiguration aktiviert ist.

**Wichtig:** Ein `AudioStream`-Objekt darf nicht aus seinem eigenen `update()`-Callback heraus zerstört werden. Die dynamische Unterstützung schützt nicht vor beliebigen Lebensdauerfehlern außerhalb der vorgesehenen Synchronisierung.

## 8. Hinweise für die Praxis

- **Audio-Blockpool dimensionieren:** `AudioMemory(n)` ist für den gesamten Graphen zuständig. FFT1024, Delays und Effekte, die Blöcke halten oder zusätzliche Blöcke erzeugen, müssen bei der Dimensionierung berücksichtigt werden.
- **PSRAM und interner RAM unterscheiden:** Der Streaming-Ringpuffer liegt im PSRAM; Audio-Blöcke, MP3-Eingabe und PCM-Decoder-Puffer sind separate Speicherbereiche. Die Größe eines Rings sagt daher nichts über die noch verfügbaren Audio-Blöcke aus.
- **Latenz korrekt einordnen:** Blockperiode, FFT-Frame-Dauer und Queue-/I/O-Timeouts beschreiben unterschiedliche Sachverhalte und sollten nicht miteinander verwechselt werden.
- **Fenster- und Sample-Puffer gültig halten:** Bei `AudioEffectGranular` und benutzerdefinierten FFT-Fenstern werden Zeiger übergeben. Der zugehörige Speicher muss über die gesamte Nutzungsdauer gültig bleiben.
- **CPU-Auslastung prüfen:** Aufwendige FIR-Filter und andere DSP-Objekte sollten im Zielsystem gemessen werden. Der Aufwand hängt von Sample-Rate, Blockgröße, Filterlänge und Gesamtheit der aktiven Objekte ab.
- **Versionsabhängige Parameter prüfen:** Nach Änderungen am Audio-Graphen, an ESP-IDF/Arduino-Core oder an den Task-Erstellungen sollten insbesondere Task-Stacks, Prioritäten, Timeouts, Queue-Längen und Puffergrößen mit dem tatsächlich gebauten Quellcode abgeglichen werden.
