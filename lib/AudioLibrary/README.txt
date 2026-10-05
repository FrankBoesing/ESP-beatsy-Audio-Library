AudioLibrary: Tasks und Timing
==============================

Tasks
-----
AudioTask       Verarbeitet alle aktiven AudioStream-Updates. Wartet auf eine
                Benachrichtigung und blockiert nicht fuer Audio-I/O. Laeuft
                auf AUDIO_PROCESSING_CORE (Standard: Core 1); dort laufen auch
                Mixer und Effekte.
AudioI2STx      Nimmt Stereo-Bloecke aus der Queue, wandelt sie in I2S-Daten um
                und schreibt sie zum I2S-Treiber. Ist nur bei I2S-Ausgabe aktiv
                und laeuft auf AUDIO_PROCESSING_CORE (Standard: Core 1).
AudioDecoder    Dekodiert MP3-Daten in PCM-Puffer. Laeuft bei Wiedergabestart
                separat auf AUDIO_DECODER_CORE (Standard: Core 0) und mit
                niedrigerer Prioritaet als AudioTask; gilt fuer MP3 und AAC.
AudioStreamRx   Liest bei Netzwerk-Streaming Daten vom HTTP-Stream und fuellt
                den AudioSourceStream-Ringpuffer. Laeuft nur bei Streams und
                ohne feste Core-Zuordnung.

                 +-------------------+
                 | AudioDecoder      |  (bei MP3)
                 +---------+---------+
                           | PCM-Puffer
                           v
                 +-------------------+       +-------------------+
                 | AudioTask         |------>| AudioI2STx        |----> I2S
                 | Stream-Updates    | Queue +-------------------+
                 +---------^---------+
                           |
            Timer oder I2S-TX-Callback

Timing
------
Ein Audio-Block umfasst standardmaessig 128 Samples. Bei 44.100 Hz dauert
ein Block 128 / 44.100 = ca. 2,90 ms. Die Sample-Rate kann geaendert werden;
die Blockperiode ist dann 128 / Sample-Rate.

Ohne I2S taktet ein periodischer Software-Timer die Updates. Mit I2S wird
dieser Timer abgeschaltet und der TX-Callback benachrichtigt AudioTask nach
einem gesendeten DMA-Block. AudioI2STx wartet bis zu 100 ms auf Queue-Daten;
der I2S-Schreibaufruf hat ebenfalls ein 100-ms-Timeout. Diese Timeouts sind
Fehler-/Leerlaufgrenzen, nicht die Audio-Blockperiode.


Puffer
------
Audio-Blockpool       AudioMemory(n) stellt n Bloecke bereit. Jeder Block
                      enthaelt 128 Mono-Samples (256 Byte PCM plus 4 Byte
                      Verwaltungsdaten, insgesamt 260 Byte). n wird vom
                      aufrufenden Programm festgelegt.
I2S-Queue             4 Stereo-Blockpaare; enthaelt Zeiger, nicht PCM-Daten.
I2S-Ausgabepuffer      1.024 Byte pro Block (128 Stereo-Frames mit je zwei
                      32-Bit-Samples), fuer die Umwandlung vor dem Schreiben.
I2S-DMA                16 Deskriptoren mit je 128 Stereo-Frames; konfiguriert
                      fuer 32-Bit-Slots, insgesamt ca. 16 KiB DMA-Nutzdaten.
MP3-Eingabe            2 KiB komprimierte Daten pro AudioPlayMp3-Objekt.
MP3-PCM                2 wechselnde Decoderpuffer mit je 2.304 int16-Samples
                      (4.608 Byte), zusammen 9.216 Byte pro MP3-Objekt.
AudioInputBuffer       Eigenstaendige, generische Buffer-Klasse; Standard sind
                      32 KiB, konfigurierbar ueber Konstruktor oder
                      AUDIO_INPUT_BUFFER_SIZE. Nutzt auf ESP32 bevorzugt
                      PSRAM. Sie wird derzeit nicht vom AudioPlayMp3-Pfad
                      verwendet.
AudioSourceStream      Separater Ringpuffer mit 128 KiB im PSRAM fuer
                      Netzwerk-Streams. Er wird von AudioStreamRx gefuellt und
                      ist unabhaengig vom AudioInputBuffer.

Netzwerk-Datenweg
-----------------
AudioStreamRx liest verfuegbare HTTP-Daten in einen lokalen 2-KiB-Chunk und
kopiert sie in den 128-KiB-AudioSourceStream-Ringpuffer im PSRAM. Der
MP3-Decoder liest jeweils verfuegbare Daten aus diesem Ring und kopiert sie in
seinen 2-KiB-Eingabepuffer im internen RAM. MP3Decode() verarbeitet diesen
Eingabepuffer und schreibt PCM in die beiden internen MP3-PCM-Puffer.

Dateien umgehen den AudioSourceStream-Ringpuffer und verwenden weiterhin den
2-KiB-MP3-Eingabepuffer. AudioInputBuffer ist eine separate, derzeit nicht in
diesen MP3-Pfaden eingesetzte Komponente. Seine Fuellregeln (Datei: 2-KiB-
Schritte, Stream: bis zum 32-KiB-Ziel) gelten nur, wenn AudioInputBuffer::fill()
explizit verwendet wird; diese Fuellziele sind keine zusaetzlichen Puffer.

DSP: Noise und Filter
---------------------
AudioSynthNoiseWhite erzeugt weisses Rauschen. amplitude() erwartet 0.0 bis
1.0. Die Verarbeitung nutzt einen 31-Bit-Park-Miller-Generator und skaliert
Samples mit 32-Bit-Arithmetik.

AudioSynthWaveformDc erzeugt einen konstanten Gleichspannungspegel von -1.0
bis 1.0. amplitude(level) setzt ihn sofort; amplitude(level, milliseconds)
fuehrt mit der aktuellen Sample-Rate weich zum neuen Pegel ueber. read() liefert
den zuletzt verarbeiteten Pegel normiert von -1.0 bis 1.0.

AudioEffectMultiply multipliziert zwei Audio-Eingangsbloecke sampleweise mit
Q15-Skalierung. Das Produkt wird in 32 Bit berechnet und vor der Rueckgabe auf
16-Bit-PCM begrenzt. Fehlt einer der beiden Bloecke, wird fuer diesen Zyklus
kein Ausgangsblock erzeugt.

AudioEffectGranular benoetigt fuer begin(sampleBank, maxLength) einen vom
Aufrufer bereitgestellten Sample-Puffer. beginFreeze() nimmt einen Grain an
einem Nulldurchgang auf und wiederholt ihn; beginPitchShift() nimmt Grains auf
und wiederholt sie mit weicher Ausblendung. setSpeed() begrenzt die
Wiedergabegeschwindigkeit auf 0.125 bis 8.0. Fuer Pitch-Shift muss der Puffer
mindestens dreimal so gross wie die maximale Grain-Laenge sein. stop() schaltet
den Effekt aus und reicht das Eingangssignal durch.

AudioFilterBiquad bietet bis zu vier kaskadierte Stufen: setLowpass(),
setHighpass(), setBandpass(), setNotch(), setLowShelf() und setHighShelf().
setCoefficients(stage, int*) akzeptiert Q2.30-Koeffizienten im Format
b0, b1, b2, a1, a2; die double*-Variante erwartet bereits normalisierte
Koeffizienten. Die Laufzeit nutzt Q30-Koeffizienten und 64-Bit-Zustaende bzw.
Akkumulatoren; das Ergebnis wird auf 16-Bit-PCM begrenzt.

AudioFilterFIR akzeptiert mit begin(short*, count) die Teensy-kompatiblen
Q15-Koeffizienten und akkumuliert Produkte in 64 Bit. Die Koeffizienten stehen
in der zeitlich rueckwaerts gespeicherten CMSIS-Reihenfolge bN-1 bis b0; die
Anzahl muss gerade und zwischen 4 und 200 liegen.
FIR_PASSTHRU reicht Audio ohne Filterung weiter; end() deaktiviert den Filter.

Die AudioStream-Schnittstelle bleibt 16-Bit-PCM. FIR-Filter mit vielen
Koeffizienten koennen deutlich CPU-Zeit beanspruchen.

## Runtime tasks

The application uses the following tasks. Stack sizes below are the values
passed to FreeRTOS by this ESP-IDF build. Priorities are expressed relative to
`configMAX_PRIORITIES` so they remain accurate if the SDK configuration changes.

| Task | Created by | Priority | Stack | Core affinity | Responsibility |
| --- | --- | --- | --- | --- | --- |
| `loopTask` | Arduino framework | 1 | 8192 bytes by default; configurable | `ARDUINO_RUNNING_CORE` | Runs `setup()` once and `loop()` repeatedly. The stream example waits for its startup prebuffer and prints diagnostics here. |
| `AudioTask` | `AudioStream::update_setup()` | `configMAX_PRIORITIES - 2` | 4096 bytes | `AUDIO_PROCESSING_CORE` (default 1) | Processes active `AudioStream` objects, including mixers and effects, after a notification. With I2S active, the I2S DMA callback provides the audio clock and notifies this task. |
| `AudioDecoder` | `AudioDecoderStream::startDecoderTask()` | `configMAX_PRIORITIES - 3` | 8192 bytes | `AUDIO_DECODER_CORE` (default 0) | Decodes MP3/AAC frames and fills the two alternating PCM buffers in internal RAM. |
| `AudioI2STx` | `AudioOutputI2S::beginInternal()` | `configMAX_PRIORITIES - 2` | 4096 bytes | `AUDIO_PROCESSING_CORE` (default 1) | Takes stereo blocks from the I2S queue, converts samples to 32-bit slots, and writes them to the I2S driver. |
| `AudioStreamRx` | `AudioSourceStream::open()` | `configMAX_PRIORITIES - 5` | 8192 bytes | Unpinned | Reads available network bytes into a 2-KiB temporary chunk and copies them into the stream ring buffer in PSRAM. Exists only for stream sources. |

The ESP-IDF also owns the Wi-Fi, TCP/IP, event-loop, timer-service, and FreeRTOS
idle tasks. Their priorities and core assignments are controlled by the
framework/SDK configuration, not by this library. The `esp_timer` service
delivers the software audio clock when software-clock mode is used; active I2S
output uses the I2S DMA callback instead. The DMA callback is an ISR, not a task.
