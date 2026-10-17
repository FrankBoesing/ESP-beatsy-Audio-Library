#pragma once

#include <Arduino.h>

enum class AudioSourceStatus : uint8_t {
    DATA,
    WOULD_BLOCK,
    END_OF_STREAM,
    ERROR
};

class AudioSource {
  public:
    virtual ~AudioSource() = default;

    virtual AudioSourceStatus read(uint8_t *buffer, size_t requested,
                                   size_t &received) = 0;

    virtual uint64_t position() const = 0;
    virtual uint64_t size() const = 0;
    virtual bool isSeekable() const = 0;
    virtual bool seek(uint64_t position) = 0;
    virtual void close() {}
    virtual bool isOpen() const = 0;

    /*
     * Minimum useful amount of data in the AudioInputBuffer.
     *
     * File:   2 KiB
     * Stream: 32 KiB
     */
    virtual size_t refillThreshold() const = 0;

    /*
     * Refill chunk / target size.
     *
     * File:
     *   one complete 2 KiB chunk is requested.
     *
     * Stream:
     *   32 KiB is the target fill level.
     */
    virtual size_t fillSize() const = 0;

    /*
     * Defines the meaning of fillSize().
     *
     * false = filesystem-style chunk fill:
     *         request one complete fillSize() chunk.
     *
     * true  = streaming-style threshold fill:
     *         request only the missing bytes up to refillThreshold().
     */
    virtual bool fillToThreshold() const = 0;
};
