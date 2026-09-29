#pragma once

#include <Arduino.h>
#include "AudioSource.h"


#ifndef AUDIO_INPUT_BUFFER_SIZE
#define AUDIO_INPUT_BUFFER_SIZE (32 * 1024)
#endif


class AudioInputBuffer
{
public:

    explicit AudioInputBuffer(
        size_t capacity = AUDIO_INPUT_BUFFER_SIZE
    );

    ~AudioInputBuffer();

    AudioInputBuffer(
        const AudioInputBuffer&
    ) = delete;

    AudioInputBuffer& operator=(
        const AudioInputBuffer&
    ) = delete;


    // ------------------------------------------------------------------------
    // Lifetime
    // ------------------------------------------------------------------------

    bool begin();

    void end();


    // ------------------------------------------------------------------------
    // Source input
    // ------------------------------------------------------------------------

    /*
     * Liest Daten von source direkt in den nächsten freien,
     * zusammenhängenden Bufferbereich.
     *
     * Es findet dabei kein zusätzlicher memcpy() statt.
     */
    AudioSourceStatus fill(
        AudioSource &source
    );


    // ------------------------------------------------------------------------
    // Read side
    // ------------------------------------------------------------------------

    /*
     * Liefert den aktuell lesbaren zusammenhängenden Bereich.
     *
     * length enthält die Anzahl der verfügbaren Bytes.
     *
     * Der Pointer bleibt gültig, bis releaseRead() aufgerufen wird
     * oder der Buffer anderweitig verändert wird.
     */
    const uint8_t *acquireRead(
        size_t &length
    ) const;

    /*
     * Gibt bereits konsumierte Bytes frei.
     *
     * Es findet kein Verschieben von Daten statt.
     */
    bool releaseRead(
        size_t consumed
    );


    // ------------------------------------------------------------------------
    // Write side
    // ------------------------------------------------------------------------

    /*
     * Liefert den nächsten freien zusammenhängenden Schreibbereich.
     *
     * length enthält die maximal mögliche Anzahl Bytes.
     */
    uint8_t *acquireWrite(
        size_t &length
    );

    /*
     * Commit der tatsächlich geschriebenen Bytes.
     */
    bool commitWrite(
        size_t written
    );


    // ------------------------------------------------------------------------
    // Information
    // ------------------------------------------------------------------------

    size_t availableRead() const;
    size_t availableWrite() const;

    size_t capacity() const;
    bool empty() const;
    bool full() const;
    bool usingPSRAM() const;


private:

    uint8_t *buffer;
    size_t buffer_size;

    /*
     * Region A = aktuell lesbare Region.
     *
     * Region B = zweite bereits geschriebene Region.
     *
     * Die logische Reihenfolge ist immer:
     *
     *      A -> B
     *
     * Erst wenn A vollständig konsumiert wurde,
     * wird B zu A.
     */

    size_t region_a_start;
    size_t region_a_length;

    size_t region_b_start;
    size_t region_b_length;


    /*
     * Zustand eines laufenden acquireWrite().
     *
     * Damit commitWrite() überprüfen kann, dass nicht
     * mehr Bytes committed werden als tatsächlich reserviert.
     */

    bool write_acquired;

    size_t write_start;
    size_t write_length;


    bool psram_allocated;


    // ------------------------------------------------------------------------
    // Internal helpers
    // ------------------------------------------------------------------------

    bool allocateBuffer();

    void freeBuffer();

    void reset();

    /*
     * Ermittelt den größten aktuell verfügbaren
     * zusammenhängenden Schreibbereich.
     */
    uint8_t *getWriteRegion(
        size_t &length
    );

    /*
     * Prüft die internen Zustandsinvarianten.
     *
     * Nur für Debugging gedacht.
     */
    bool validate() const;
};
