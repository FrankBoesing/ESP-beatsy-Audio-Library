#pragma once

#include "AudioSource.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <HTTPClient.h>
#include "optimize.h"

class AudioSourceStream : public AudioSource {
  public:
    static constexpr size_t BUFFER_SIZE = 128U * 1024U;
    static constexpr size_t ICY_METADATA_MAX_SIZE = 255U * 2;

    AudioSourceStream();
    explicit AudioSourceStream(Stream &stream);
    ~AudioSourceStream() override;

    bool open(HTTPClient &http);
    bool open(Stream &stream, uint32_t icyMetaInt = 0);

    // Copies the latest raw ICY metadata block, without a trailing NUL.
    // If capacity is too small, size receives the required size and the
    // pending block remains available for a later call.
    int takeIcyMetadata(uint8_t *buffer, size_t capacity);

    AudioSourceStatus read(uint8_t *buffer, size_t requested, size_t &received) override;

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
    size_t bufferedBytes() const;

  private:
    static constexpr size_t PRODUCER_CHUNK_SIZE = 4096;
    static constexpr uint32_t NETWORK_IDLE_TIMEOUT_MS = 15000;
    static constexpr uint32_t STREAM_READ_TIMEOUT_MS = 500;
    static constexpr size_t REFILLTRESHOLD = 32 * 1024;

    static void producerTaskEntry(void *arg);
    void producerTaskLoop();
    bool checkNetworkTimeout();

    Stream *_stream = nullptr;
    uint64_t _position;
    uint32_t _lastDataMs = 0;
    bool _streamError = false;
    uint8_t *_buffer = nullptr;
    size_t _readIndex = 0;
    size_t _writeIndex = 0;
    size_t _bufferedBytes = 0;
    bool _stopRequested = false;
    TaskHandle_t _producerTask = nullptr;
    mutable portMUX_TYPE _bufferMux = portMUX_INITIALIZER_UNLOCKED;

    struct {
        uint32_t MetaInt = 0;
        uint32_t AudioRemaining = 0;
        size_t MetadataExpected = 0;
        size_t MetadataReceived = 0;
        size_t MetadataReadySize = 0;
        uint8_t *MetadataWriteBuffer = nullptr;
        uint8_t *MetadataReadyBuffer = nullptr;
        SemaphoreHandle_t MetadataMutex = nullptr;
        bool NeedLength = true;
    } _icy;
};
