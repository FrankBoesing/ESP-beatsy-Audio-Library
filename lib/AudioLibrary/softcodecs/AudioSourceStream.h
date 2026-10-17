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
    static constexpr size_t ICY_STREAMTITLE_MAX_SIZE = 256U;

    AudioSourceStream();
    explicit AudioSourceStream(Stream &stream);
    ~AudioSourceStream() override;

    bool open(HTTPClient &http);
    bool open(Stream &stream, uint32_t icyMetaInt = 0);

    // Copies the latest changed StreamTitle.
    //
    // Returns:
    //   > 0 : number of bytes copied, excluding the trailing NUL
    //     0 : no new StreamTitle available
    //    -1 : error or buffer too small
    //
    // If the buffer is too small, the pending title remains available
    // for a later call.
    int takeIcyStreamTitle(char *buffer, size_t capacity);

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
    static constexpr size_t ICY_METADATA_PARSE_CHUNK_SIZE = 128;
    static constexpr uint32_t NETWORK_IDLE_TIMEOUT_MS = 15000;
    static constexpr uint32_t STREAM_READ_TIMEOUT_MS = 500;
    static constexpr size_t REFILLTRESHOLD = 32 * 1024;

    static void producerTaskEntry(void *arg);
    void producerTaskLoop();
    bool checkNetworkTimeout();

    void resetIcyMetadataParser();
    void parseIcyMetadata(const uint8_t *buffer, size_t size);
    void finishIcyMetadata();

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

    enum class IcyParseState : uint8_t { Search, Collect, Done };
    struct {
        uint32_t MetaInt = 0;
        uint32_t AudioRemaining = 0;
        size_t MetadataExpected = 0;
        size_t MetadataReceived = 0;

        IcyParseState State = IcyParseState::Search;
        size_t KeyMatch = 0;
        size_t StreamTitleWriteSize = 0;
        bool StreamTitleTooLong = false;

        size_t StreamTitleSize = 0;
        bool StreamTitlePending = false;

        uint8_t *StreamTitleWriteBuffer = nullptr;
        uint8_t *StreamTitleReadyBuffer = nullptr;

        SemaphoreHandle_t MetadataMutex = nullptr;
        bool NeedLength = true;
    } _icy;
};
