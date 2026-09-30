#pragma once

#include "AudioSource.h"
#include <Arduino.h>

class AudioSourceStream : public AudioSource {
  public:
    AudioSourceStream();
    explicit AudioSourceStream(Stream &stream);

    bool open(Stream &stream);

    AudioSourceStatus read(uint8_t *buffer, size_t requested,
                           size_t &received) override;

    uint64_t position() const override;
    uint64_t size() const override;

    bool isSeekable() const override;

    bool seek(uint64_t position) override;

    void close() override;

    bool isOpen() const override;

    size_t refillThreshold() const override;
    size_t fillSize() const override;
    bool fillToThreshold() const override;

    Stream *stream();

  private:
    Stream *_stream;
    uint64_t _position;
};
