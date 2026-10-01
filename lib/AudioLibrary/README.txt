AudioLibrary: Tasks und Timing
==============================

Tasks
-----
AudioTask       Verarbeitet alle aktiven AudioStream-Updates. Wartet auf eine
                Benachrichtigung und blockiert nicht fuer Audio-I/O.
AudioI2STx      Nimmt Stereo-Bloecke aus der Queue, wandelt sie in I2S-Daten um
                und schreibt sie zum I2S-Treiber. Ist nur bei I2S-Ausgabe aktiv.
AudioDecoder    Dekodiert MP3-Daten in PCM-Puffer. Laeuft bei Wiedergabestart
                separat und mit niedrigerer Prioritaet als AudioTask.

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
AudioInputBuffer       Standardmaessig 32 KiB pro Quellpuffer; Kapazitaet ist
                      konfigurierbar und kann in PSRAM liegen.

Die Quellen fuellen diesen letzten Puffer unterschiedlich: Dateien in
2-KiB-Schritten, Streams bis zum 32-KiB-Ziel. Das sind Fuellmengen, keine
zusaetzlichen Puffer.
