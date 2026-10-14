#include <Arduino.h>
#include "output_record.h"
#include <esp_heap_caps.h>
#include <stdio.h>
#include <string.h>

namespace {
const uint16_t WAV_PCM_FORMAT = 1;
const uint32_t WAV_RIFF_LIMIT = 0xFFFFFFFFUL;
}

// ============================================================================
// Construction / lifetime
// ============================================================================

AudioOutputRecord::AudioOutputRecord()
    : AudioStream(2, inputQueueArray) {}

AudioOutputRecord::~AudioOutputRecord() {
    stop();

    if (_writerWake != nullptr) {
        vSemaphoreDelete(_writerWake);
        _writerWake = nullptr;
    }

    if (_writerDone != nullptr) {
        vSemaphoreDelete(_writerDone);
        _writerDone = nullptr;
    }

    if (_buffer != nullptr) {
        heap_caps_free(_buffer);
        _buffer = nullptr;
    }

    if (_file) {
        _file.close();
    }
}

bool AudioOutputRecord::setBufferSize(size_t bytes) {
    if (bytes < AUDIO_BLOCK_SAMPLES * sizeof(int16_t) * 2U || (bytes & 3U) != 0) {
        return false;
    }

    bool changed = false;

    portENTER_CRITICAL(&_bufferMux);
    if (!_workerRunning && !_recording && _buffer == nullptr) {
        _bufferSize = bytes;
        changed = true;
    }
    portEXIT_CRITICAL(&_bufferMux);

    return changed;
}

// ============================================================================
// Start
// ============================================================================

bool AudioOutputRecord::begin(fs::FS &filesystem, const char *basePath,
                              uint32_t sampleRate, uint8_t channels,
                              int8_t writerCore) {
    if (basePath == nullptr || basePath[0] == '\0' ||
        (channels != 1 && channels != 2) ||
        writerCore < CORE_UNPINNED || writerCore >= portNUM_PROCESSORS) {
        return false;
    }

    if (sampleRate == 0 || sampleRate > WAV_RIFF_LIMIT / (channels * sizeof(int16_t))) {
        return false;
    }

    bool busy = false;
    bool finishing = false;
    portENTER_CRITICAL(&_bufferMux);
    busy = _workerRunning || _recording || _buffer != nullptr;
    finishing = _completionPending;
    portEXIT_CRITICAL(&_bufferMux);
    if (busy) {
        return false;
    }

    if (_writerDone == nullptr) {
        _writerDone = xSemaphoreCreateBinary();
        if (_writerDone == nullptr) {
            return false;
        }
    }

    if (_writerWake == nullptr) {
        _writerWake = xSemaphoreCreateBinary();
        if (_writerWake == nullptr) {
            return false;
        }
    }

    // A task which has already stopped may still have a completion signal to
    // consume. Wait for it before reusing the object and its semaphore.
    if (finishing) {
        if (xSemaphoreTake(_writerDone, portMAX_DELAY) != pdTRUE) {
            return false;
        }
        portENTER_CRITICAL(&_bufferMux);
        _completionPending = false;
        portEXIT_CRITICAL(&_bufferMux);
    } else {
        xSemaphoreTake(_writerDone, 0);
    }

    portENTER_CRITICAL(&_bufferMux);
    busy = _workerRunning || _completionPending || _recording || _buffer != nullptr;
    portEXIT_CRITICAL(&_bufferMux);
    if (busy) {
        return false;
    }

    xSemaphoreTake(_writerWake, 0);

    const int tempLength = snprintf(_tempPath, sizeof(_tempPath), "%s.tmp", basePath);
    const int finalLength = snprintf(_finalPath, sizeof(_finalPath), "%s.wav", basePath);
    if (tempLength <= 0 || (size_t)tempLength >= sizeof(_tempPath) ||
        finalLength <= 0 || (size_t)finalLength >= sizeof(_finalPath)) {
        return false;
    }

    // Never overwrite an old final file or an unfinished recording.
    if (filesystem.exists(_tempPath) || filesystem.exists(_finalPath)) {
        return false;
    }

    if (!psramFound()) {
        return false;
    }

    uint8_t *ringBuffer =
        (uint8_t *)heap_caps_malloc(_bufferSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ringBuffer == nullptr) {
        return false;
    }

    _filesystem = &filesystem;
    _sampleRate = sampleRate;
    _channels = channels;
    _writerCore = writerCore;
    _dataBytesWritten = 0;
    _dataBytesAccepted = 0;
    _dataSizeOffset = 0;
    _dataOffset = 0;
    _maxDataBytes = 0;

    _file = filesystem.open(_tempPath, FILE_WRITE);
    if (!_file || !writeInitialHeader()) {
        if (_file) {
            _file.close();
        }
        filesystem.remove(_tempPath);
        heap_caps_free(ringBuffer);
        _filesystem = nullptr;
        return false;
    }

    _buffer = ringBuffer;

    portENTER_CRITICAL(&_bufferMux);
    _writeIndex = 0;
    _readIndex = 0;
    _bufferedBytes = 0;
    _stopRequested = false;
    _producerActive = false;
    _hasError = false;
    _lastResult = false;
    _recording = false;
    _workerRunning = true;
    portEXIT_CRITICAL(&_bufferMux);

    BaseType_t result;
    if (_writerCore == CORE_UNPINNED) {
        result = xTaskCreate(&AudioOutputRecord::writerTaskEntry, "AudioRecord",
                             6144, this, configMAX_PRIORITIES - 5, nullptr);
    } else {
        result = xTaskCreatePinnedToCore(&AudioOutputRecord::writerTaskEntry, "AudioRecord",
                                         6144, this, configMAX_PRIORITIES - 5,
                                         nullptr, _writerCore);
    }

    if (result != pdPASS) {
        _file.close();
        filesystem.remove(_tempPath);
        heap_caps_free(_buffer);
        _buffer = nullptr;
        _filesystem = nullptr;

        portENTER_CRITICAL(&_bufferMux);
        _recording = false;
        _workerRunning = false;
        _hasError = true;
        _lastResult = false;
        portEXIT_CRITICAL(&_bufferMux);
        return false;
    }

    // The writer task is alive and the header is complete before the audio
    // graph starts passing samples into the ring buffer.
    portENTER_CRITICAL(&_bufferMux);
    _recording = true;
    portEXIT_CRITICAL(&_bufferMux);

    return true;
}

// ============================================================================
// WAV header
// ============================================================================

bool AudioOutputRecord::writeBytes(const void *data, size_t size) {
    if (size == 0) {
        return true;
    }

    return _file.write((const uint8_t *)data, size) == size;
}

bool AudioOutputRecord::writeFourCC(const char *id) {
    return writeBytes(id, 4);
}

bool AudioOutputRecord::writeLE16(uint16_t value) {
    uint8_t bytes[2];
    bytes[0] = value;
    bytes[1] = value >> 8;
    return writeBytes(bytes, sizeof(bytes));
}

bool AudioOutputRecord::writeLE32(uint32_t value) {
    uint8_t bytes[4];
    bytes[0] = value;
    bytes[1] = value >> 8;
    bytes[2] = value >> 16;
    bytes[3] = value >> 24;
    return writeBytes(bytes, sizeof(bytes));
}

bool AudioOutputRecord::writeInitialHeader() {
    const uint16_t blockAlign = _channels * sizeof(int16_t);
    const uint32_t byteRate = _sampleRate * blockAlign;

    if (!writeFourCC("RIFF") || !writeLE32(WAV_SIZE_PLACEHOLDER) ||
        !writeFourCC("WAVE") ||
        !writeFourCC("fmt ") || !writeLE32(16) ||
        !writeLE16(WAV_PCM_FORMAT) || !writeLE16(_channels) ||
        !writeLE32(_sampleRate) || !writeLE32(byteRate) ||
        !writeLE16(blockAlign) || !writeLE16(16) ||
        !writeFourCC("data")) {
        return false;
    }

    _dataSizeOffset = _file.position();
    if (!writeLE32(WAV_SIZE_PLACEHOLDER)) {
        return false;
    }
    _dataOffset = _file.position();

    // A standard PCM WAV header is exactly 44 bytes.
    if (_dataOffset != 44U) {
        return false;
    }

    // RIFF size excludes the first 8 bytes ("RIFF" and its size field).
    _maxDataBytes = (uint64_t)WAV_RIFF_LIMIT - (_dataOffset - 8U);
    return true;
}

bool AudioOutputRecord::writeFinalHeader() {
    _file.flush();

    const uint64_t fileSize = (uint64_t)_dataOffset + _dataBytesWritten;
    if (_dataBytesWritten > WAV_RIFF_LIMIT || fileSize < 8U ||
        fileSize - 8U > WAV_RIFF_LIMIT) {
        return false;
    }

    const uint32_t riffSize = fileSize - 8U;
    const uint32_t dataSize = _dataBytesWritten;

    if (!_file.seek(4) || !writeLE32(riffSize) ||
        !_file.seek(_dataSizeOffset) || !writeLE32(dataSize)) {
        return false;
    }

    _file.flush();
    return true;
}

// ============================================================================
// Ring buffer helpers
// ============================================================================

void AudioOutputRecord::copyIntoRing(size_t index, const uint8_t *source, size_t size) {
    size_t first = _bufferSize - index;
    if (first > size) {
        first = size;
    }

    memcpy(_buffer + index, source, first);
    if (size > first) {
        memcpy(_buffer, source + first, size - first);
    }
}

void AudioOutputRecord::copyFromRing(size_t index, uint8_t *destination, size_t size) {
    size_t first = _bufferSize - index;
    if (first > size) {
        first = size;
    }

    memcpy(destination, _buffer + index, first);
    if (size > first) {
        memcpy(destination + first, _buffer, size - first);
    }
}

// ============================================================================
// Audio graph input
// ============================================================================

void AudioOutputRecord::update() {
    audio_block_t *left = receiveReadOnly(0);
    audio_block_t *right = receiveReadOnly(1);

    bool acceptBlock = false;
    bool notifyWriter = false;
    size_t writeIndex = 0;
    size_t bytesToCopy = 0;
    uint8_t channels = 0;

    portENTER_CRITICAL(&_bufferMux);

    if (_recording && _buffer != nullptr) {
        channels = _channels;
        bytesToCopy = AUDIO_BLOCK_SAMPLES * channels * sizeof(int16_t);

        if (_bufferSize - _bufferedBytes < bytesToCopy ||
            _dataBytesAccepted + bytesToCopy > _maxDataBytes) {
            // Never block the audio update waiting for the SD card.
            _recording = false;
            _stopRequested = true;
            _hasError = true;
            notifyWriter = true;
        } else {
            _producerActive = true;
            writeIndex = _writeIndex;
            acceptBlock = true;
        }
    }

    portEXIT_CRITICAL(&_bufferMux);

    if (acceptBlock) {
        const int16_t *leftSamples = left != nullptr ? left->data : audio_zeroblock;

        if (channels == 1) {
            copyIntoRing(writeIndex, (const uint8_t *)leftSamples, bytesToCopy);
        } else {
            const int16_t *rightSamples = right != nullptr ? right->data : audio_zeroblock;
            int16_t interleaved[AUDIO_BLOCK_SAMPLES * 2];

            for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
                interleaved[i * 2] = leftSamples[i];
                interleaved[i * 2 + 1] = rightSamples[i];
            }

            copyIntoRing(writeIndex, (const uint8_t *)interleaved, bytesToCopy);
        }

        portENTER_CRITICAL(&_bufferMux);
        _writeIndex = (writeIndex + bytesToCopy) % _bufferSize;
        _bufferedBytes += bytesToCopy;
        _dataBytesAccepted += bytesToCopy;
        _producerActive = false;
        portEXIT_CRITICAL(&_bufferMux);

        // Notify every accepted block. This avoids a lost wakeup if the writer
        // empties the buffer while update() is copying the next block.
        notifyWriter = true;
    }

    if (notifyWriter && _writerWake != nullptr) {
        xSemaphoreGive(_writerWake);
    }

    if (left != nullptr) {
        release(left);
    }
    if (right != nullptr) {
        release(right);
    }
}

// ============================================================================
// Writer task
// ============================================================================

void AudioOutputRecord::writerTaskEntry(void *parameter) {
    AudioOutputRecord *self = (AudioOutputRecord *)parameter;
    self->writerTaskLoop();
    vTaskDelete(nullptr);
}

void AudioOutputRecord::writerTaskLoop() {
    uint8_t chunk[WRITER_CHUNK_SIZE];
    bool writeFailed = false;

    for (;;) {
        size_t readIndex = 0;
        size_t bytesAvailable = 0;
        bool stopRequested = false;
        bool producerActive = false;

        portENTER_CRITICAL(&_bufferMux);
        readIndex = _readIndex;
        bytesAvailable = _bufferedBytes;
        stopRequested = _stopRequested;
        producerActive = _producerActive;
        portEXIT_CRITICAL(&_bufferMux);

        // Once the SD write itself fails, preserve the .tmp file and wait only
        // for any in-flight producer copy to finish before freeing PSRAM.
        if (writeFailed && !producerActive) {
            break;
        }

        if (!writeFailed && bytesAvailable > 0) {
            size_t bytesToWrite = bytesAvailable;
            if (bytesToWrite > sizeof(chunk)) {
                bytesToWrite = sizeof(chunk);
            }

            copyFromRing(readIndex, chunk, bytesToWrite);
            const size_t written = _file.write(chunk, bytesToWrite);

            if (written > 0) {
                portENTER_CRITICAL(&_bufferMux);
                _readIndex = (_readIndex + written) % _bufferSize;
                _bufferedBytes -= written;
                portEXIT_CRITICAL(&_bufferMux);

                _dataBytesWritten += written;
            }

            if (written != bytesToWrite) {
                writeFailed = true;

                portENTER_CRITICAL(&_bufferMux);
                _hasError = true;
                _recording = false;
                _stopRequested = true;
                portEXIT_CRITICAL(&_bufferMux);
            }

            continue;
        }

        if (stopRequested && bytesAvailable == 0 && !producerActive) {
            break;
        }

        xSemaphoreTake(_writerWake, portMAX_DELAY);
    }

    bool hasError = false;
    portENTER_CRITICAL(&_bufferMux);
    hasError = _hasError;
    portEXIT_CRITICAL(&_bufferMux);

    bool success = false;

    if (!writeFailed && !hasError && writeFinalHeader()) {
        _file.close();

        if (_filesystem != nullptr && _filesystem->rename(_tempPath, _finalPath)) {
            success = true;
        }
    } else {
        _file.close();
    }

    if (_buffer != nullptr) {
        heap_caps_free(_buffer);
        _buffer = nullptr;
    }

    portENTER_CRITICAL(&_bufferMux);
    _recording = false;
    _workerRunning = false;
    _completionPending = true;
    _stopRequested = false;
    _producerActive = false;
    _bufferedBytes = 0;
    _writeIndex = 0;
    _readIndex = 0;
    _hasError = !success;
    _lastResult = success;
    _filesystem = nullptr;
    portEXIT_CRITICAL(&_bufferMux);

    // stop() or the next begin() consumes this signal before clearing
    // _completionPending. No instance members are accessed after this point.
    xSemaphoreGive(_writerDone);
}

// ============================================================================
// Stop / status
// ============================================================================

bool AudioOutputRecord::stop() {
    bool running = false;
    bool finishing = false;
    bool notifyWriter = false;

    portENTER_CRITICAL(&_bufferMux);

    running = _workerRunning;
    finishing = _completionPending;

    if (running) {
        _recording = false;
        if (!_stopRequested) {
            _stopRequested = true;
            notifyWriter = true;
        }
    }

    portEXIT_CRITICAL(&_bufferMux);

    if (!running) {
        if (_writerDone == nullptr) {
            return _lastResult;
        }

        if (finishing) {
            if (xSemaphoreTake(_writerDone, portMAX_DELAY) != pdTRUE) {
                return false;
            }
            portENTER_CRITICAL(&_bufferMux);
            _completionPending = false;
            portEXIT_CRITICAL(&_bufferMux);
        } else {
            xSemaphoreTake(_writerDone, 0);
        }

        return _lastResult;
    }

    if (notifyWriter && _writerWake != nullptr) {
        xSemaphoreGive(_writerWake);
    }

    if (_writerDone == nullptr ||
        xSemaphoreTake(_writerDone, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    portENTER_CRITICAL(&_bufferMux);
    _completionPending = false;
    portEXIT_CRITICAL(&_bufferMux);

    return _lastResult;
}

bool AudioOutputRecord::isRecording() const {
    bool recording;

    portENTER_CRITICAL(&_bufferMux);
    recording = _recording;
    portEXIT_CRITICAL(&_bufferMux);

    return recording;
}

bool AudioOutputRecord::hasError() const {
    bool error;

    portENTER_CRITICAL(&_bufferMux);
    error = _hasError;
    portEXIT_CRITICAL(&_bufferMux);

    return error;
}
