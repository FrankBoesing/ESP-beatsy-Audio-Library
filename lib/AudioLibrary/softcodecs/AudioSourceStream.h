#pragma once

#include "AudioSource.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "optimize.h"

class AudioSourceStream : public AudioSource {
  public:
    static constexpr size_t BUFFER_SIZE = 128U * 1024U;

    AudioSourceStream();
    explicit AudioSourceStream(Stream &stream);
    ~AudioSourceStream() override;

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
    uint32_t wouldBlockCount() const;
    uint32_t networkWaitMs() const;
    size_t bufferedBytes() const;
    uint64_t receivedBytes() const;

  private:
    static constexpr size_t PRODUCER_CHUNK_SIZE = 2048;
    static constexpr uint32_t NETWORK_IDLE_TIMEOUT_MS = 15000;
    static constexpr uint32_t STREAM_READ_TIMEOUT_MS = 500;

    static void producerTaskEntry(void *arg);
    void producerTaskLoop();
    bool recordNetworkWait();

    Stream *_stream;
    uint64_t _position;
    uint64_t _receivedBytes = 0;
    uint32_t _wouldBlockCount = 0;
    uint32_t _networkWaitMs = 0;
    uint32_t _lastDataMs = 0;
    bool _streamError = false;
    uint8_t *_buffer = nullptr;
    size_t _readIndex = 0;
    size_t _writeIndex = 0;
    size_t _bufferedBytes = 0;
    bool _stopRequested = false;
    TaskHandle_t _producerTask = nullptr;
    portMUX_TYPE _bufferMux = portMUX_INITIALIZER_UNLOCKED;
};
