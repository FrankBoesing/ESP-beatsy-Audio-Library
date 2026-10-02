#include "AudioSourceStream.h"

#include <algorithm>

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
#endif

AudioSourceStream::AudioSourceStream() : _stream(nullptr), _position(0) {}

AudioSourceStream::AudioSourceStream(Stream &stream)
    : _stream(nullptr), _position(0) {
    open(stream);
}

AudioSourceStream::~AudioSourceStream() {
    close();
}

bool AudioSourceStream::open(Stream &stream) {
    close();

#if defined(ARDUINO_ARCH_ESP32)
    if (!psramFound()) {
        return false;
    }

    _buffer = static_cast<uint8_t *>(
        heap_caps_malloc(BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#else
    _buffer = static_cast<uint8_t *>(malloc(BUFFER_SIZE));
#endif

    if (_buffer == nullptr) {
        return false;
    }

    portENTER_CRITICAL(&_bufferMux);
    _stream = &stream;
    _position = 0;
    _receivedBytes = 0;
    _wouldBlockCount = 0;
    _networkWaitMs = 0;
    _readIndex = 0;
    _writeIndex = 0;
    _bufferedBytes = 0;
    _stopRequested = false;
    portEXIT_CRITICAL(&_bufferMux);

    const BaseType_t result = xTaskCreate(
        &AudioSourceStream::producerTaskEntry, "AudioStreamRx", 8192, this,
        configMAX_PRIORITIES - 5, &_producerTask);

    if (result != pdPASS) {
        close();
        return false;
    }

    return true;
}

AudioSourceStatus AudioSourceStream::read(uint8_t *buffer, size_t requested,
                                          size_t &received) {
    received = 0;

    if (!_buffer || !_stream) {
        return AudioSourceStatus::ERROR;
    }

    if (!buffer || requested == 0) {
        return AudioSourceStatus::ERROR;
    }

    size_t readIndex;
    size_t bytesToRead;

    portENTER_CRITICAL(&_bufferMux);
    bytesToRead = std::min(requested, _bufferedBytes);
    readIndex = _readIndex;
    if (bytesToRead == 0) {
        ++_wouldBlockCount;
        portEXIT_CRITICAL(&_bufferMux);
        return AudioSourceStatus::WOULD_BLOCK;
    }
    portEXIT_CRITICAL(&_bufferMux);

    const size_t firstChunk =
        std::min(bytesToRead, BUFFER_SIZE - readIndex);
    memcpy(buffer, _buffer + readIndex, firstChunk);
    if (bytesToRead > firstChunk) {
        memcpy(buffer + firstChunk, _buffer, bytesToRead - firstChunk);
    }

    portENTER_CRITICAL(&_bufferMux);
    _readIndex = (_readIndex + bytesToRead) % BUFFER_SIZE;
    _bufferedBytes -= bytesToRead;
    _position += bytesToRead;
    portEXIT_CRITICAL(&_bufferMux);

    received = bytesToRead;
    return AudioSourceStatus::DATA;
}

uint64_t AudioSourceStream::position() const {
    portENTER_CRITICAL(const_cast<portMUX_TYPE *>(&_bufferMux));
    const uint64_t currentPosition = _position;
    portEXIT_CRITICAL(const_cast<portMUX_TYPE *>(&_bufferMux));
    return currentPosition;
}

uint64_t AudioSourceStream::size() const {
    return 0;
}

bool AudioSourceStream::isSeekable() const {
    return false;
}

bool AudioSourceStream::seek(uint64_t position) {
    (void)position;
    return false;
}

void AudioSourceStream::close() {
    portENTER_CRITICAL(&_bufferMux);
    _stopRequested = true;
    bool producerRunning = _producerTask != nullptr;
    portEXIT_CRITICAL(&_bufferMux);

    while (producerRunning) {
        vTaskDelay(1);
        portENTER_CRITICAL(&_bufferMux);
        producerRunning = _producerTask != nullptr;
        portEXIT_CRITICAL(&_bufferMux);
    }

    portENTER_CRITICAL(&_bufferMux);
    _stream = nullptr;
    _position = 0;
    _receivedBytes = 0;
    _readIndex = 0;
    _writeIndex = 0;
    _bufferedBytes = 0;
    portEXIT_CRITICAL(&_bufferMux);

#if defined(ARDUINO_ARCH_ESP32)
    heap_caps_free(_buffer);
#else
    free(_buffer);
#endif
    _buffer = nullptr;
}

bool AudioSourceStream::isOpen() const {
    return _stream != nullptr;
}

size_t AudioSourceStream::refillThreshold() const {
    return 32 * 1024;
}

size_t AudioSourceStream::fillSize() const {
    return 32 * 1024;
}

bool AudioSourceStream::fillToThreshold() const {
    return true;
}

Stream *AudioSourceStream::stream() {
    return _stream;
}

uint32_t AudioSourceStream::wouldBlockCount() const {
    portENTER_CRITICAL(const_cast<portMUX_TYPE *>(&_bufferMux));
    const uint32_t count = _wouldBlockCount;
    portEXIT_CRITICAL(const_cast<portMUX_TYPE *>(&_bufferMux));
    return count;
}

uint32_t AudioSourceStream::networkWaitMs() const {
    portENTER_CRITICAL(const_cast<portMUX_TYPE *>(&_bufferMux));
    const uint32_t waitMs = _networkWaitMs;
    portEXIT_CRITICAL(const_cast<portMUX_TYPE *>(&_bufferMux));
    return waitMs;
}

size_t AudioSourceStream::bufferedBytes() const {
    portENTER_CRITICAL(const_cast<portMUX_TYPE *>(&_bufferMux));
    const size_t buffered = _bufferedBytes;
    portEXIT_CRITICAL(const_cast<portMUX_TYPE *>(&_bufferMux));
    return buffered;
}

uint64_t AudioSourceStream::receivedBytes() const {
    portENTER_CRITICAL(const_cast<portMUX_TYPE *>(&_bufferMux));
    const uint64_t received = _receivedBytes;
    portEXIT_CRITICAL(const_cast<portMUX_TYPE *>(&_bufferMux));
    return received;
}

void AudioSourceStream::producerTaskEntry(void *arg) {
    static_cast<AudioSourceStream *>(arg)->producerTaskLoop();
}

void AudioSourceStream::producerTaskLoop() {
    uint8_t chunk[PRODUCER_CHUNK_SIZE];

    for (;;) {
        size_t writeIndex;
        size_t freeBytes;
        bool stopRequested;

        portENTER_CRITICAL(&_bufferMux);
        stopRequested = _stopRequested;
        writeIndex = _writeIndex;
        freeBytes = BUFFER_SIZE - _bufferedBytes;
        freeBytes = std::min(freeBytes, BUFFER_SIZE - writeIndex);
        portEXIT_CRITICAL(&_bufferMux);

        if (stopRequested) {
            break;
        }

        if (freeBytes == 0) {
            vTaskDelay(1);
            continue;
        }

        const int available = _stream->available();
        if (available <= 0) {
            portENTER_CRITICAL(&_bufferMux);
            _networkWaitMs += 2;
            portEXIT_CRITICAL(&_bufferMux);
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        const size_t requested = std::min(
            std::min(freeBytes, static_cast<size_t>(available)),
            sizeof(chunk));
        const size_t received = _stream->readBytes(
            reinterpret_cast<char *>(chunk), requested);

        if (received == 0) {
            portENTER_CRITICAL(&_bufferMux);
            _networkWaitMs += 2;
            portEXIT_CRITICAL(&_bufferMux);
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        memcpy(_buffer + writeIndex, chunk, received);

        portENTER_CRITICAL(&_bufferMux);
        _writeIndex = (_writeIndex + received) % BUFFER_SIZE;
        _bufferedBytes += received;
        _receivedBytes += received;
        portEXIT_CRITICAL(&_bufferMux);
    }

    portENTER_CRITICAL(&_bufferMux);
    _producerTask = nullptr;
    portEXIT_CRITICAL(&_bufferMux);
    vTaskDelete(nullptr);
}
