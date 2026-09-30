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

    /*
     * Öffentlicher Reset:
     * setzt nur den logischen Bufferzustand zurück.
     * Der reservierte Speicher bleibt erhalten.
     */
    void reset();

    // ------------------------------------------------------------------------
    // Source input
    // ------------------------------------------------------------------------

    AudioSourceStatus fill(
        AudioSource &source
    );

    // ------------------------------------------------------------------------
    // Read side
    // ------------------------------------------------------------------------

    const uint8_t *acquireRead(
        size_t &length
    ) const;

    bool releaseRead(
        size_t consumed
    );

    // ------------------------------------------------------------------------
    // Write side
    // ------------------------------------------------------------------------

    uint8_t *acquireWrite(
        size_t &length
    );

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

    size_t region_a_start;
    size_t region_a_length;

    size_t region_b_start;
    size_t region_b_length;

    bool write_acquired;

    size_t write_start;
    size_t write_length;

    bool psram_allocated;

    bool allocateBuffer();
    void freeBuffer();

    uint8_t *getWriteRegion(
        size_t &length
    );

    bool validate() const;
};
