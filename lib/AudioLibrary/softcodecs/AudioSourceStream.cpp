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
bool AudioSourceStream::open(Stream &stream, uint32_t icyMetaInt) {
    close();

#if defined(ARDUINO_ARCH_ESP32)
    if (!psramFound()) {
        return false;
    }

    _buffer = (uint8_t *)heap_caps_malloc(BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    _buffer = (uint8_t *)malloc(BUFFER_SIZE);
#endif

    if (_buffer == nullptr) {
        return false;
    }

    if (icyMetaInt > 0) {
#if defined(ARDUINO_ARCH_ESP32)
        _icyMetadataWriteBuffer =
            (uint8_t *)heap_caps_malloc(ICY_METADATA_MAX_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        _icyMetadataReadyBuffer =
            (uint8_t *)heap_caps_malloc(ICY_METADATA_MAX_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
        _icyMetadataWriteBuffer = (uint8_t *)malloc(ICY_METADATA_MAX_SIZE);
        _icyMetadataReadyBuffer = (uint8_t *)malloc(ICY_METADATA_MAX_SIZE);
#endif
        _icyMetadataMutex = xSemaphoreCreateMutex();
        if (_icyMetadataWriteBuffer == nullptr || _icyMetadataReadyBuffer == nullptr ||
            _icyMetadataMutex == nullptr) {
            close();
            return false;
        }
    }

    uint32_t now = millis();

    portENTER_CRITICAL(&_bufferMux);
    _stream = &stream;
    _position = 0;
    _lastDataMs = now;
    _streamError = false;
    _readIndex = 0;
    _writeIndex = 0;
    _bufferedBytes = 0;
    _stopRequested = false;
    portEXIT_CRITICAL(&_bufferMux);

    _icyMetaInt = icyMetaInt;
    _icyAudioRemaining = icyMetaInt;
    _icyMetadataExpected = 0;
    _icyMetadataReceived = 0;
    _icyNeedLength = true;
    _icyMetadataReadySize = 0;

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

bool AudioSourceStream::takeIcyMetadata(uint8_t *buffer, size_t capacity, size_t &size) {
    size = 0;
    if (_icyMetadataMutex == nullptr || (buffer == nullptr && capacity > 0)) {
        return false;
    }

    if (xSemaphoreTake(_icyMetadataMutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    const size_t pendingSize = _icyMetadataReadySize;
    if (pendingSize == 0) {
        xSemaphoreGive(_icyMetadataMutex);
        return false;
    }

    size = pendingSize;
    if (buffer == nullptr || capacity < pendingSize) {
        xSemaphoreGive(_icyMetadataMutex);
        return false;
    }

    memcpy(buffer, _icyMetadataReadyBuffer, pendingSize);
    _icyMetadataReadySize = 0;
    xSemaphoreGive(_icyMetadataMutex);
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
    _readIndex = 0;
    _writeIndex = 0;
    _bufferedBytes = 0;
    _lastDataMs = 0;
    _streamError = false;

    portEXIT_CRITICAL(&_bufferMux);

    if (_icyMetadataMutex != nullptr) {
        vSemaphoreDelete(_icyMetadataMutex);
        _icyMetadataMutex = nullptr;
    }

#if defined(ARDUINO_ARCH_ESP32)
    heap_caps_free(_buffer);
    heap_caps_free(_icyMetadataWriteBuffer);
    heap_caps_free(_icyMetadataReadyBuffer);
#else
    free(_buffer);
    free(_icyMetadataWriteBuffer);
    free(_icyMetadataReadyBuffer);
#endif

    _buffer = nullptr;
    _icyMetadataWriteBuffer = nullptr;
    _icyMetadataReadyBuffer = nullptr;
    _icyMetadataReadySize = 0;
    _icyMetaInt = 0;
    _icyAudioRemaining = 0;
    _icyMetadataExpected = 0;
    _icyMetadataReceived = 0;
    _icyNeedLength = true;
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

bool AudioSourceStream::checkNetworkTimeout() {
    const uint32_t now = millis();

    portENTER_CRITICAL(&_bufferMux);
    const bool timedOut = now - _lastDataMs >= NETWORK_IDLE_TIMEOUT_MS;

    if (timedOut) {
        _streamError = true;
    }

    portEXIT_CRITICAL(&_bufferMux);

    return timedOut;
}

size_t AudioSourceStream::bufferedBytes() const {
    portENTER_CRITICAL(&_bufferMux);
    const size_t buffered = _bufferedBytes;
    portEXIT_CRITICAL(&_bufferMux);

    return buffered;
}

void AudioSourceStream::producerTaskEntry(void *arg) {
    ((AudioSourceStream *)arg)->producerTaskLoop();
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
            if (checkNetworkTimeout()) {
                break;
            }

            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        if (_icyMetaInt > 0 && _icyAudioRemaining == 0) {
            if (_icyNeedLength) {
                uint8_t lengthUnits = 0;
                const size_t received = stream->readBytes(
                    (char *)&lengthUnits, sizeof(lengthUnits));
                if (received == 0) {
                    if (checkNetworkTimeout()) {
                        break;
                    }

                    vTaskDelay(pdMS_TO_TICKS(5));
                    continue;
                }

                const uint32_t now = millis();
                portENTER_CRITICAL(&_bufferMux);
                _lastDataMs = now;
                portEXIT_CRITICAL(&_bufferMux);

                _icyMetadataExpected = lengthUnits * 16U;
                _icyMetadataReceived = 0;
                _icyNeedLength = false;
                if (_icyMetadataExpected == 0) {
                    _icyAudioRemaining = _icyMetaInt;
                    _icyNeedLength = true;
                }
                continue;
            }

            const size_t metadataRemaining =
                _icyMetadataExpected - _icyMetadataReceived;
            const size_t availableBytes = available;
            const size_t metadataChunk = std::min(
                metadataRemaining, std::min(availableBytes, PRODUCER_CHUNK_SIZE));
            const size_t received = stream->readBytes(
                (char *)(_icyMetadataWriteBuffer + _icyMetadataReceived),
                metadataChunk);

            if (received == 0) {
                if (checkNetworkTimeout()) {
                    break;
                }

                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }

            const uint32_t now = millis();
            portENTER_CRITICAL(&_bufferMux);
            _lastDataMs = now;
            portEXIT_CRITICAL(&_bufferMux);

            _icyMetadataReceived += received;
            if (_icyMetadataReceived == _icyMetadataExpected) {
                if (xSemaphoreTake(_icyMetadataMutex, portMAX_DELAY) != pdTRUE) {
                    break;
                }
                std::swap(_icyMetadataWriteBuffer, _icyMetadataReadyBuffer);
                _icyMetadataReadySize = _icyMetadataExpected;
                xSemaphoreGive(_icyMetadataMutex);

                _icyMetadataExpected = 0;
                _icyMetadataReceived = 0;
                _icyAudioRemaining = _icyMetaInt;
                _icyNeedLength = true;
            }
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
        const size_t availableBytes = available;
        size_t requested = std::min(freeBytes, availableBytes);
        if (_icyMetaInt > 0) {
            const size_t icyAudioRemaining = _icyAudioRemaining;
            requested = std::min(requested, icyAudioRemaining);
        }
        const size_t received = stream->readBytes((char *)(_buffer + writeIndex), requested);

        if (received == 0) {
            if (checkNetworkTimeout()) {
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
        if (_icyMetaInt > 0) {
            _icyAudioRemaining -= received;
        }
        portEXIT_CRITICAL(&_bufferMux);
    }

    portENTER_CRITICAL(&_bufferMux);
    _producerTask = nullptr;
    portEXIT_CRITICAL(&_bufferMux);

    vTaskDelete(nullptr);
}
