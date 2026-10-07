#include "AudioSourceStream.h"
#include <algorithm>

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
#endif

AudioSourceStream::AudioSourceStream() : _stream(nullptr), _position(0) {}

AudioSourceStream::AudioSourceStream(Stream &stream) : _stream(nullptr), _position(0) { open(stream); }

AudioSourceStream::~AudioSourceStream() { close(); }

OSIZE
bool AudioSourceStream::open(HTTPClient &http) {
    const long metaInt = http.header("icy-metaint").toInt();
    return open(http.getStream(), (uint32_t)(metaInt < 0 ? 0 : metaInt));
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
        _icy.MetadataWriteBuffer =
            (uint8_t *)heap_caps_malloc(ICY_METADATA_MAX_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        _icy.MetadataReadyBuffer =
            (uint8_t *)heap_caps_malloc(ICY_METADATA_MAX_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
        _icy.MetadataWriteBuffer = (uint8_t *)malloc(ICY_METADATA_MAX_SIZE);
        _icy.MetadataReadyBuffer = (uint8_t *)malloc(ICY_METADATA_MAX_SIZE);
#endif
        _icy.MetadataMutex = xSemaphoreCreateMutex();
        if (_icy.MetadataWriteBuffer == nullptr || _icy.MetadataReadyBuffer == nullptr || _icy.MetadataMutex == nullptr) {
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

    _icy.MetaInt = icyMetaInt;
    _icy.AudioRemaining = icyMetaInt;
    _icy.MetadataExpected = 0;
    _icy.MetadataReceived = 0;
    _icy.NeedLength = true;
    _icy.MetadataReadySize = 0;

    stream.setTimeout(STREAM_READ_TIMEOUT_MS);

    const BaseType_t result = xTaskCreate(&AudioSourceStream::producerTaskEntry, "AudioStreamRx",
                                          3072, // Stack
                                          this, configMAX_PRIORITIES - 5, &_producerTask);

    if (result != pdPASS) {
        close();
        return false;
    }

    return true;
}

int AudioSourceStream::takeIcyMetadata(uint8_t *buffer, size_t capacity) {
    if (_icy.MetadataMutex == nullptr || buffer == nullptr || capacity == 0) {
        return -1;
    }

    if (xSemaphoreTake(_icy.MetadataMutex, portMAX_DELAY) != pdTRUE) {
        return -1;
    }

    const size_t size = _icy.MetadataReadySize;

    if (size == 0 || size >= capacity) {
        xSemaphoreGive(_icy.MetadataMutex);
        return -1;
    }

    memcpy(buffer, _icy.MetadataReadyBuffer, size);
    buffer[size] = '\0';
    _icy.MetadataReadySize = 0;

    xSemaphoreGive(_icy.MetadataMutex);
    return static_cast<int>(size);
}

AudioSourceStatus AudioSourceStream::read(uint8_t *buffer, size_t requested, size_t &received) {
    received = 0;

    if (!_buffer || !_stream || !buffer || requested == 0) {
        return AudioSourceStatus::ERROR;
    }

    size_t readIndex;
    size_t bytesToRead;

    portENTER_CRITICAL(&_bufferMux);

    bytesToRead = std::min(requested, _bufferedBytes);
    if (bytesToRead == 0) {
        const bool streamError = _streamError;
        portEXIT_CRITICAL(&_bufferMux);
        return streamError ? AudioSourceStatus::ERROR : AudioSourceStatus::WOULD_BLOCK;
    }
    readIndex = _readIndex;
    portEXIT_CRITICAL(&_bufferMux);

    const size_t firstChunk = std::min(bytesToRead, BUFFER_SIZE - readIndex);
    memcpy(buffer, _buffer + readIndex, firstChunk);
    if (bytesToRead > firstChunk) {
        memcpy(buffer + firstChunk, _buffer, bytesToRead - firstChunk);
    }

    portENTER_CRITICAL(&_bufferMux);
    _readIndex += bytesToRead;
    if (_readIndex >= BUFFER_SIZE) _readIndex -= BUFFER_SIZE;
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

uint64_t AudioSourceStream::size() const { return 0; }
bool AudioSourceStream::isSeekable() const { return false; }
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
        vTaskDelay(pdMS_TO_TICKS(2));
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

    if (_icy.MetadataMutex != nullptr) {
        vSemaphoreDelete(_icy.MetadataMutex);
        _icy.MetadataMutex = nullptr;
    }

#if defined(ARDUINO_ARCH_ESP32)
    heap_caps_free(_buffer);
    heap_caps_free(_icy.MetadataWriteBuffer);
    heap_caps_free(_icy.MetadataReadyBuffer);
#else
    free(_buffer);
    free(_icy.MetadataWriteBuffer);
    free(_icy.MetadataReadyBuffer);
#endif

    _buffer = nullptr;
    _icy = {};
    _icy.NeedLength = true;
}

bool AudioSourceStream::isOpen() const { return _stream != nullptr; }
size_t AudioSourceStream::refillThreshold() const { return REFILLTRESHOLD; }
size_t AudioSourceStream::fillSize() const { return REFILLTRESHOLD; }
bool AudioSourceStream::fillToThreshold() const { return true; }

Stream *AudioSourceStream::stream() { return _stream; }

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

void AudioSourceStream::producerTaskEntry(void *arg) { ((AudioSourceStream *)arg)->producerTaskLoop(); }
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
            vTaskDelay(pdMS_TO_TICKS(5));
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

        if (_icy.MetaInt > 0 && _icy.AudioRemaining == 0) {
            if (_icy.NeedLength) {
                uint8_t lengthUnits = 0;
                const size_t received = stream->readBytes((char *)&lengthUnits, sizeof(lengthUnits));
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

                _icy.MetadataExpected = lengthUnits * 16U;
                _icy.MetadataReceived = 0;
                _icy.NeedLength = false;
                if (_icy.MetadataExpected == 0) {
                    _icy.AudioRemaining = _icy.MetaInt;
                    _icy.NeedLength = true;
                }
                continue;
            }

            const size_t metadataRemaining = _icy.MetadataExpected - _icy.MetadataReceived;
            const size_t availableBytes = available;
            const size_t metadataChunk = std::min(metadataRemaining, std::min(availableBytes, PRODUCER_CHUNK_SIZE));
            const size_t received =
                stream->readBytes((char *)(_icy.MetadataWriteBuffer + _icy.MetadataReceived), metadataChunk);

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

            _icy.MetadataReceived += received;
            if (_icy.MetadataReceived == _icy.MetadataExpected) {
                if (xSemaphoreTake(_icy.MetadataMutex, portMAX_DELAY) != pdTRUE) {
                    break;
                }
                std::swap(_icy.MetadataWriteBuffer, _icy.MetadataReadyBuffer);
                _icy.MetadataReadySize = _icy.MetadataExpected;
                xSemaphoreGive(_icy.MetadataMutex);

                _icy.MetadataExpected = 0;
                _icy.MetadataReceived = 0;
                _icy.AudioRemaining = _icy.MetaInt;
                _icy.NeedLength = true;
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
        if (_icy.MetaInt > 0) {
            const size_t icyAudioRemaining = _icy.AudioRemaining;
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
        if (_writeIndex == BUFFER_SIZE) _writeIndex = 0;
        _bufferedBytes += received;
        if (_icy.MetaInt > 0) {
            _icy.AudioRemaining -= received;
        }
        portEXIT_CRITICAL(&_bufferMux);
    }

    portENTER_CRITICAL(&_bufferMux);
    _producerTask = nullptr;
    portEXIT_CRITICAL(&_bufferMux);

    vTaskDelete(nullptr);
}
