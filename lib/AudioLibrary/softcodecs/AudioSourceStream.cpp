#include "AudioSourceStream.h"
#include <algorithm>
#include <cstring>

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

    if (_buffer == nullptr) return false;
    if (icyMetaInt > 0) {
#if defined(ARDUINO_ARCH_ESP32)
        uint8_t *buffer =
            (uint8_t *)heap_caps_malloc(ICY_STREAMTITLE_MAX_SIZE * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
        uint8_t *buffer = (uint8_t *)malloc(ICY_STREAMTITLE_MAX_SIZE * 2);
#endif
        _icy.StreamTitleWriteBuffer = buffer;
        _icy.StreamTitleReadyBuffer = buffer + ICY_STREAMTITLE_MAX_SIZE;
        _icy.MetadataMutex = xSemaphoreCreateMutex();

        if (_icy.StreamTitleWriteBuffer == nullptr || _icy.MetadataMutex == nullptr) {
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
    resetIcyMetadataParser();
    _icy.StreamTitleSize = 0;
    _icy.StreamTitlePending = false;

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

OSIZE
void AudioSourceStream::resetIcyMetadataParser() {
    _icy.State = IcyParseState::Search;
    _icy.KeyMatch = 0;
    _icy.StreamTitleWriteSize = 0;
    _icy.StreamTitleTooLong = false;
    if (_icy.StreamTitleWriteBuffer != nullptr) {
        _icy.StreamTitleWriteBuffer[0] = '\0';
    }
}

void AudioSourceStream::parseIcyMetadata(const uint8_t *buffer, size_t size) {
    static constexpr char KEY[] = "StreamTitle='";
    static constexpr size_t KEY_LENGTH = sizeof(KEY) - 1;

    for (size_t i = 0; i < size; ++i) {
        const uint8_t c = buffer[i];

        if (_icy.State == IcyParseState::Done) {
            continue;
        }

        if (_icy.State == IcyParseState::Collect) {
            if (c == '\'') {
                _icy.State = IcyParseState::Done;
                continue;
            }

            if (_icy.StreamTitleWriteSize < ICY_STREAMTITLE_MAX_SIZE - 1) {
                _icy.StreamTitleWriteBuffer[_icy.StreamTitleWriteSize++] = c;
            } else {
                _icy.StreamTitleTooLong = true;
            }

            continue;
        }

        // Search for "StreamTitle='"
        if (c == static_cast<uint8_t>(KEY[_icy.KeyMatch])) {
            ++_icy.KeyMatch;

            if (_icy.KeyMatch == KEY_LENGTH) {
                _icy.State = IcyParseState::Collect;
                _icy.KeyMatch = 0;
            }
        } else {
            // The key starts with 'S'. If the current byte is an 'S',
            // it can be the beginning of a new match.
            _icy.KeyMatch = (c == static_cast<uint8_t>(KEY[0])) ? 1 : 0;
        }
    }
}

void AudioSourceStream::finishIcyMetadata() {
    if (_icy.State != IcyParseState::Done || _icy.StreamTitleTooLong) {
        resetIcyMetadataParser();
        return;
    }

    const size_t size = _icy.StreamTitleWriteSize;

    if (xSemaphoreTake(_icy.MetadataMutex, portMAX_DELAY) != pdTRUE) {
        resetIcyMetadataParser();
        return;
    }

    const bool changed =
        size != _icy.StreamTitleSize || memcmp(_icy.StreamTitleReadyBuffer, _icy.StreamTitleWriteBuffer, size) != 0;

    if (changed) {
        std::swap(_icy.StreamTitleWriteBuffer, _icy.StreamTitleReadyBuffer);
        _icy.StreamTitleSize = size;
        _icy.StreamTitlePending = true;
    }

    xSemaphoreGive(_icy.MetadataMutex);
    resetIcyMetadataParser();
}

int AudioSourceStream::takeIcyStreamTitle(char *buffer, size_t capacity) {
    if (_icy.MetadataMutex == nullptr || buffer == nullptr || capacity == 0) {
        return -1;
    }

    if (xSemaphoreTake(_icy.MetadataMutex, portMAX_DELAY) != pdTRUE) {
        return -1;
    }

    if (!_icy.StreamTitlePending) {
        xSemaphoreGive(_icy.MetadataMutex);
        return 0;
    }

    const size_t size = _icy.StreamTitleSize;

    // One additional byte is required for the trailing NUL.
    if (size + 1 > capacity) {
        xSemaphoreGive(_icy.MetadataMutex);
        return -1;
    }

    memcpy(buffer, _icy.StreamTitleReadyBuffer, size);
    buffer[size] = '\0';
    _icy.StreamTitlePending = false;
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

    if (_readIndex >= BUFFER_SIZE) {
        _readIndex -= BUFFER_SIZE;
    }
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
    heap_caps_free(_icy.StreamTitleWriteBuffer);
#else
    free(_buffer);
    free(_icy.StreamTitleWriteBuffer);
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
    if (timedOut) _streamError = true;
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
    // Small temporary buffer for incremental ICY metadata parsing.
    // The complete metadata block is never stored.
    uint8_t metadataBuffer[ICY_METADATA_PARSE_CHUNK_SIZE];

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

        if (stopRequested) break;
        if (freeBytes == 0) { // Puffer ist voll
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        Stream *stream = _stream;
        if (stream == nullptr) {
            break;
        }

        const int available = stream->available();
        if (available <= 0) { // No Data
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
                resetIcyMetadataParser();

                if (_icy.MetadataExpected == 0) {
                    _icy.AudioRemaining = _icy.MetaInt;
                    _icy.NeedLength = true;
                }

                continue;
            }

            const size_t metadataRemaining = _icy.MetadataExpected - _icy.MetadataReceived;
            const size_t availableBytes = (size_t)available;
            const size_t metadataChunk =
                std::min(metadataRemaining, std::min(availableBytes, ICY_METADATA_PARSE_CHUNK_SIZE));
            const size_t received = stream->readBytes((char *)metadataBuffer, metadataChunk);

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

            parseIcyMetadata(metadataBuffer, received);
            _icy.MetadataReceived += received;

            if (_icy.MetadataReceived == _icy.MetadataExpected) {
                finishIcyMetadata();
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
         * Because freeBytes is already limited to the contiguous
         * region up to BUFFER_SIZE, no wrap-around write can occur here.
         */
        const size_t availableBytes = static_cast<size_t>(available);
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

        if (_icy.MetaInt > 0) {
            _icy.AudioRemaining -= received;
        }
        uint32_t now = millis();
        portENTER_CRITICAL(&_bufferMux);
        _lastDataMs = now;
        _writeIndex += received;
        if (_writeIndex >= BUFFER_SIZE) {
            _writeIndex = 0;
        }
        _bufferedBytes += received;
        portEXIT_CRITICAL(&_bufferMux);
    }

    portENTER_CRITICAL(&_bufferMux);
    _producerTask = nullptr;
    portEXIT_CRITICAL(&_bufferMux);

    vTaskDelete(nullptr);
}
