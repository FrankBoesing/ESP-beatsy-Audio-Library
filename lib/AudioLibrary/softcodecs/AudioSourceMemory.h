#pragma once

#include "AudioSource.h"

class AudioSourceMemory : public AudioSource {
public:
    AudioSourceMemory();

    AudioSourceMemory(
        const uint8_t *data,
        size_t size
    );

    bool open(
        const uint8_t *data,
        size_t size
    );

    AudioSourceStatus read(
        uint8_t *buffer,
        size_t requested,
        size_t &received
    ) override;

    uint64_t position() const override;
    uint64_t size() const override;

    bool isSeekable() const override;

    bool seek(uint64_t position) override;

    void close() override;

    bool isOpen() const override;

private:
    const uint8_t *_data;
    size_t _size;
    size_t _position;
};
