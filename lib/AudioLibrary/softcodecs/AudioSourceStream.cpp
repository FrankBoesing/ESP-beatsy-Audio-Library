#include "AudioSourceStream.h"
#include <algorithm>

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
#endif

AudioSourceStream::AudioSourceStream()
    : _stream(nullptr), _position(0) {}

AudioSourceStream::AudioSourceStream(Stream &stream)
    : _stream(nullptr), _position(0) {
    open(stream);
}

AudioSourceStream::~AudioSourceStream() {
    close();
}

OSIZE
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

    uint32_t now = millis();

    portENTER_CRITICAL(&_bufferMux);
    _stream = &stream;
    _position = 0;
    _receivedBytes = 0;
    _wouldBlockCount = 0;
    _networkWaitMs = 0;
    _lastDataMs = now;
    _streamError = false;
    _readIndex = 0;
    _writeIndex = 0;
    _bufferedBytes = 0;
    _stopRequested = false;
    portEXIT_CRITICAL(&_bufferMux);

    stream.setTimeout(STREAM_READ_TIMEOUT_MS);

    const BaseType_t result = xTaskCreate(
        &AudioSourceStream::producerTaskEntry, "AudioStreamRx",
        3072, // Stack
        this, configMAX_PRIORITIES - 5, &_producerTask);

    if (result != pdPASS) {
        close();
        return false;
    }

    return true;
}

AudioSourceStatus AudioSourceStream::read(uint8_t *buffer,
                                          size_t requested,
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
    if (bytesToRead == 0) {
        ++_wouldBlockCount;
        const bool streamError = _streamError;
        portEXIT_CRITICAL(&_bufferMux);
        return streamError ? AudioSourceStatus::ERROR
                           : AudioSourceStatus::WOULD_BLOCK;
    }
    readIndex = _readIndex;
    portEXIT_CRITICAL(&_bufferMux);

    const size_t firstChunk = std::min(bytesToRead, BUFFER_SIZE - readIndex);
    memcpy(buffer, _buffer + readIndex, firstChunk);
    if (bytesToRead > firstChunk) {
        memcpy(buffer + firstChunk,
               _buffer,
               bytesToRead - firstChunk);
    }

    portENTER_CRITICAL(&_bufferMux);
    _readIndex += bytesToRead;
    if (_readIndex >= BUFFER_SIZE)
        _readIndex -= BUFFER_SIZE;
    _bufferedBytes -= bytesToRead;
    _position += bytesToRead;
    portEXIT_CRITICAL(&_bufferMux);

    received = bytesToRead;

    return AudioSourceStatus::DATA;
}

uint64_t AudioSourceStream::position() const {
    portENTER_CRITICAL(&_bufferMux);
    const uint64_t currentPosition = _position;
    portEXIT_CRITICAL(&_bufferMux);

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

OSIZE
void AudioSourceStream::close() {
    portENTER_CRITICAL(&_bufferMux);
    _stopRequested = true;
    bool producerRunning = _producerTask != nullptr;
    portEXIT_CRITICAL(&_bufferMux);

    while (producerRunning) {
        vTaskDelay(2);
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
    _lastDataMs = 0;
    _streamError = false;

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

bool AudioSourceStream::recordNetworkWait() {
    const uint32_t now = millis();

    portENTER_CRITICAL(&_bufferMux);
    _networkWaitMs += 2;

    const bool timedOut = now - _lastDataMs >= NETWORK_IDLE_TIMEOUT_MS;

    if (timedOut) {
        _streamError = true;
    }

    portEXIT_CRITICAL(&_bufferMux);

    return timedOut;
}

uint32_t AudioSourceStream::wouldBlockCount() const {
    portENTER_CRITICAL(&_bufferMux);
    const uint32_t count = _wouldBlockCount;
    portEXIT_CRITICAL(&_bufferMux);

    return count;
}

uint32_t AudioSourceStream::networkWaitMs() const {
    portENTER_CRITICAL(&_bufferMux);
    const uint32_t waitMs = _networkWaitMs;
    portEXIT_CRITICAL(&_bufferMux);

    return waitMs;
}

size_t AudioSourceStream::bufferedBytes() const {
    portENTER_CRITICAL(&_bufferMux);
    const size_t buffered = _bufferedBytes;
    portEXIT_CRITICAL(&_bufferMux);

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
    for (;;) {
        size_t writeIndex;
        size_t freeBytes;
        bool stopRequested;

        /*
         * Determine the amount of contiguous free space at the current
         * write position.
         *
         * We deliberately limit freeBytes to the end of the buffer.
         * This guarantees that readBytes() can write directly into the
         * ring buffer without crossing the wrap boundary.
         */
        portENTER_CRITICAL(&_bufferMux);
        stopRequested = _stopRequested;
        writeIndex = _writeIndex;
        freeBytes = std::min(BUFFER_SIZE - _bufferedBytes, BUFFER_SIZE - writeIndex);
        portEXIT_CRITICAL(&_bufferMux);

        if (stopRequested) {
            break;
        }

        if (freeBytes == 0) { //Puffer ist voll
            vTaskDelay(5);
            continue;
        }

        Stream *stream = _stream;

        if (stream == nullptr) {
            break;
        }

        const int available = stream->available();

        if (available <= 0) { //No Data
            if (recordNetworkWait()) {
                break;
            }

            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        /*
         * Read directly into the ring buffer.
         *
         * The previous implementation first read into a temporary
         * stack buffer and then copied the data into _buffer.
         *
         * Because freeBytes is already limited to the contiguous
         * region up to BUFFER_SIZE, no wrap-around write can occur here.
         */
        const size_t requested = std::min(freeBytes, static_cast<size_t>(available));
        const size_t received = stream->readBytes(reinterpret_cast<char *>(_buffer + writeIndex),requested);

        if (received == 0) {
            if (recordNetworkWait()) {
                break;
            }

            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        uint32_t now = millis();
        portENTER_CRITICAL(&_bufferMux);
        _lastDataMs = now;
        _writeIndex += received;
        if (_writeIndex == BUFFER_SIZE)
            _writeIndex = 0;
        _bufferedBytes += received;
        _receivedBytes += received;
        portEXIT_CRITICAL(&_bufferMux);
    }

    portENTER_CRITICAL(&_bufferMux);
    _producerTask = nullptr;
    portEXIT_CRITICAL(&_bufferMux);

    vTaskDelete(nullptr);
}
