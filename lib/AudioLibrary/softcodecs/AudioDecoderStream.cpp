#include "AudioDecoderStream.h"
#include "defines.h"

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
#endif

#include <algorithm>
#include <cstring>

namespace {
constexpr const char *TAG = "AudioDecoderStream";
}

AudioDecoderStream::AudioDecoderStream(size_t pcmBufferSamples)
    : AudioStream(0, nullptr), _pcmBufferSamples(pcmBufferSamples) {
    /*
     * No task creation and no decoding here.
     */
}

AudioDecoderStream::~AudioDecoderStream() {
    stopDecoderTask();
}

bool AudioDecoderStream::allocatePcmBuffers() {
    if (_pcmBufferSamples == 0) {
        return false;
    }

    const size_t bytes = _pcmBufferSamples * sizeof(int16_t);

    for (uint8_t i = 0; i < PCM_BUFFER_COUNT; ++i) {
#if defined(ARDUINO_ARCH_ESP32)
        _pcm[i] = static_cast<int16_t *>(
            heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

        if (_pcm[i] == nullptr && psramFound()) {
            _pcm[i] = static_cast<int16_t *>(
                heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        }
#else
        _pcm[i] = static_cast<int16_t *>(malloc(bytes));
#endif

        if (_pcm[i] == nullptr) {
            freePcmBuffers();
            return false;
        }
    }

    return true;
}

void AudioDecoderStream::freePcmBuffers() {
    for (uint8_t i = 0; i < PCM_BUFFER_COUNT; ++i) {
        if (_pcm[i] == nullptr) {
            continue;
        }

#if defined(ARDUINO_ARCH_ESP32)
        heap_caps_free(_pcm[i]);
#else
        free(_pcm[i]);
#endif

        _pcm[i] = nullptr;
    }
}

void AudioDecoderStream::clearDecoderState() {
    portENTER_CRITICAL(&_decoderMux);

    _pcmState[0] = PCM_FREE;
    _pcmState[1] = PCM_FREE;

    _pcmSamples[0] = 0;
    _pcmSamples[1] = 0;

    _readBuffer = -1;
    _readPosition = 0;

    _decoderStopRequested = false;
    _decoderTaskRunning = false;
    _decoderFinished = false;
    _decoderError = 0;
    _updateRunning = false;

    _samplesPlayed = 0;

    _decoderTask = nullptr;

    portEXIT_CRITICAL(&_decoderMux);
}

bool AudioDecoderStream::startDecoderTask() {
    /*
     * startDecoderTask() is only allowed to allocate new PCM storage once
     * no previous owner is using the old buffers.
     */
    portENTER_CRITICAL(&_decoderMux);
    const bool busy = _decoderTaskRunning || _updateRunning;
    portEXIT_CRITICAL(&_decoderMux);

    if (busy) {
        return false;
    }

    freePcmBuffers();
    clearDecoderState();

    if (!allocatePcmBuffers()) {
        return false;
    }

    /*
     * Keep the stream in the AudioStream update list, but mark it active only
     * after the decoder resources and task have been created successfully.
     */
    active = true;
    _decoderTaskRunning = true;

    const BaseType_t result = xTaskCreate(
        &AudioDecoderStream::decoderTaskEntry, "AudioDecoder",
        DECODER_TASK_STACK, this, DECODER_TASK_PRIORITY, &_decoderTask);

    if (result != pdPASS) {
        active = false;
        _decoderTaskRunning = false;
        _decoderTask = nullptr;
        freePcmBuffers();
        clearDecoderState();
        return false;
    }

    ESP_LOGI(TAG, "decoder task started: priority=%lu, PCM samples/buffer=%u",
             static_cast<unsigned long>(DECODER_TASK_PRIORITY),
             static_cast<unsigned>(_pcmBufferSamples));

    return true;
}

void AudioDecoderStream::stopDecoderTask() {
    /*
     * Non-realtime shutdown path.
     *
     * 'active = false' prevents future scheduler iterations from selecting
     * this stream. A scheduler iteration that has already entered update()
     * is synchronized separately by _updateRunning.
     */
    active = false;

    TaskHandle_t task = nullptr;

    portENTER_CRITICAL(&_decoderMux);

    const bool taskRunning = _decoderTaskRunning;
    task = _decoderTask;

    /*
     * This flag closes the race with update():
     *
     * - update() checks it and refuses to start a new PCM access after stop
     *   has begun.
     * - an update() that already set _updateRunning=true is allowed to finish,
     *   and stopDecoderTask() waits for it.
     */
    _decoderStopRequested = true;

    portEXIT_CRITICAL(&_decoderMux);

    if (task != nullptr && taskRunning) {
        xTaskNotifyGive(task);
    }

    /*
     * IMPORTANT:
     * Do not use a timeout here followed by freePcmBuffers().
     *
     * If either owner were still alive at the timeout point, freeing the PCM
     * buffers would recreate the exact use-after-free reported by the review.
     *
     * The decoder task itself only performs cooperative waits, and update()
     * is deliberately non-blocking, so the non-realtime shutdown path waits
     * until both owners have definitely released the buffers.
     */
    for (;;) {
        bool decoderRunning;
        bool updateRunning;

        portENTER_CRITICAL(&_decoderMux);

        decoderRunning = _decoderTaskRunning;
        updateRunning = _updateRunning;

        portEXIT_CRITICAL(&_decoderMux);

        if (!decoderRunning && !updateRunning) {
            break;
        }

        vTaskDelay(1);
    }

    /*
     * At this point neither the decoder task nor update() can dereference
     * _pcm[] anymore.
     */
    freePcmBuffers();
    clearDecoderState();
}

int AudioDecoderStream::claimFreeBuffer() {
    portENTER_CRITICAL(&_decoderMux);

    for (uint8_t i = 0; i < PCM_BUFFER_COUNT; ++i) {
        if (_pcmState[i] == PCM_FREE) {
            _pcmState[i] = PCM_FILLING;
            _pcmSamples[i] = 0;

            portEXIT_CRITICAL(&_decoderMux);
            return static_cast<int>(i);
        }
    }

    portEXIT_CRITICAL(&_decoderMux);
    return -1;
}

void AudioDecoderStream::releaseFilledBuffer(uint8_t index) {
    if (index >= PCM_BUFFER_COUNT) {
        return;
    }

    portENTER_CRITICAL(&_decoderMux);

    _pcmSamples[index] = 0;
    _pcmState[index] = PCM_FREE;

    portEXIT_CRITICAL(&_decoderMux);
}

void AudioDecoderStream::publishReadyBuffer(uint8_t index, size_t samples) {
    if (index >= PCM_BUFFER_COUNT) {
        return;
    }

    portENTER_CRITICAL(&_decoderMux);

    _pcmSamples[index] = samples;
    _pcmState[index] = PCM_READY;

    portEXIT_CRITICAL(&_decoderMux);
}

int AudioDecoderStream::acquireReadyBuffer() {
    portENTER_CRITICAL(&_decoderMux);

    if (_readBuffer >= 0) {
        const int result = _readBuffer;
        portEXIT_CRITICAL(&_decoderMux);
        return result;
    }

    for (uint8_t i = 0; i < PCM_BUFFER_COUNT; ++i) {
        if (_pcmState[i] == PCM_READY && _pcmSamples[i] > 0) {
            _readBuffer = static_cast<int8_t>(i);
            _readPosition = 0;

            const int result = i;
            portEXIT_CRITICAL(&_decoderMux);
            return result;
        }
    }

    portEXIT_CRITICAL(&_decoderMux);
    return -1;
}

void AudioDecoderStream::releaseReadBuffer(uint8_t index) {
    if (index >= PCM_BUFFER_COUNT) {
        return;
    }

    portENTER_CRITICAL(&_decoderMux);

    _pcmSamples[index] = 0;
    _pcmState[index] = PCM_FREE;

    _readBuffer = -1;
    _readPosition = 0;

    portEXIT_CRITICAL(&_decoderMux);
}

void AudioDecoderStream::setDecoderFinished(int errorCode) {
    portENTER_CRITICAL(&_decoderMux);

    _decoderFinished = true;
    _decoderError = errorCode;

    portEXIT_CRITICAL(&_decoderMux);
}

bool AudioDecoderStream::decoderTaskRunning() const {
    return _decoderTaskRunning;
}

bool AudioDecoderStream::decoderFinished() const {
    return _decoderFinished;
}

int AudioDecoderStream::decoderError() const {
    return _decoderError;
}

void AudioDecoderStream::decoderTaskEntry(void *arg) {
    static_cast<AudioDecoderStream *>(arg)->decoderTaskLoop();
}

void AudioDecoderStream::decoderTaskLoop() {
    for (;;) {
        portENTER_CRITICAL(&_decoderMux);
        const bool stopRequested = _decoderStopRequested;
        portEXIT_CRITICAL(&_decoderMux);

        if (stopRequested) {
            break;
        }

        const int bufferIndex = claimFreeBuffer();

        if (bufferIndex < 0) {
            vTaskDelay(1);
            continue;
        }

        size_t produced = 0;

        const DecodeResult result =
            decodePcmBuffer(_pcm[bufferIndex], _pcmBufferSamples, produced);

        if (produced > _pcmBufferSamples) {
            ESP_LOGE(TAG, "codec returned too many PCM samples: %u > %u",
                     static_cast<unsigned>(produced),
                     static_cast<unsigned>(_pcmBufferSamples));

            releaseFilledBuffer(static_cast<uint8_t>(bufferIndex));
            setDecoderFinished(-1);
            break;
        }

        /*
         * All codecs in this layer use interleaved stereo PCM.
         */
        if ((produced & 1U) != 0) {
            ESP_LOGE(TAG, "codec returned odd interleaved PCM sample count: %u",
                     static_cast<unsigned>(produced));

            releaseFilledBuffer(static_cast<uint8_t>(bufferIndex));
            setDecoderFinished(-1);
            break;
        }

        switch (result) {
        case DecodeResult::FILLED:

            if (produced > 0) {
                publishReadyBuffer(static_cast<uint8_t>(bufferIndex), produced);
            } else {
                releaseFilledBuffer(static_cast<uint8_t>(bufferIndex));
            }

            vTaskDelay(1);
            break;

        case DecodeResult::RETRY:

            releaseFilledBuffer(static_cast<uint8_t>(bufferIndex));

            vTaskDelay(1);
            break;

        case DecodeResult::END_OF_STREAM:

            if (produced > 0) {
                publishReadyBuffer(static_cast<uint8_t>(bufferIndex), produced);
            } else {
                releaseFilledBuffer(static_cast<uint8_t>(bufferIndex));
            }

            setDecoderFinished(0);
            goto decoder_exit;

        case DecodeResult::ERROR:
        default:

            if (produced > 0) {
                publishReadyBuffer(static_cast<uint8_t>(bufferIndex), produced);
            } else {
                releaseFilledBuffer(static_cast<uint8_t>(bufferIndex));
            }

            setDecoderFinished(-1);
            goto decoder_exit;
        }
    }

decoder_exit:

    portENTER_CRITICAL(&_decoderMux);
    _decoderTaskRunning = false;
    _decoderTask = nullptr;
    portEXIT_CRITICAL(&_decoderMux);

    ESP_LOGI(TAG, "decoder task stopped");

    vTaskDelete(nullptr);
}

OSPEED
void AudioDecoderStream::update() {
    /*
     * Synchronize entry against stopDecoderTask().
     *
     * If stop has already started, do not touch _pcm[] at all.
     * If update wins this critical section first, stopDecoderTask() sees
     * _updateRunning=true and waits until this invocation has finished.
     */

    if (_decoderStopRequested) {
        return;
    }

    portENTER_CRITICAL(&_decoderMux);
    _updateRunning = true;
    portEXIT_CRITICAL(&_decoderMux);

    audio_block_t *left = allocate();

    if (left == nullptr) {
        portENTER_CRITICAL(&_decoderMux);
        _updateRunning = false;
        portEXIT_CRITICAL(&_decoderMux);
        return;
    }

    audio_block_t *right = allocate();

    if (right == nullptr) {
        release(left);

        portENTER_CRITICAL(&_decoderMux);
        _updateRunning = false;
        portEXIT_CRITICAL(&_decoderMux);
        return;
    }

    size_t outputFrames = 0;

    while (outputFrames < AUDIO_BLOCK_SAMPLES) {
        const int index = acquireReadyBuffer();

        if (index < 0) {
            break;
        }

        const size_t totalSamples = _pcmSamples[index];

        if (_readPosition >= totalSamples) {
            releaseReadBuffer(static_cast<uint8_t>(index));
            continue;
        }

        const size_t availableSamples = totalSamples - _readPosition;
        const size_t availableFrames = availableSamples / 2U;

        if (availableFrames == 0) {
            releaseReadBuffer(static_cast<uint8_t>(index));
            continue;
        }

        const size_t frames =
            std::min(availableFrames,
                     static_cast<size_t>(AUDIO_BLOCK_SAMPLES - outputFrames));

        const int16_t *source = _pcm[index] + _readPosition;

#pragma GCC unroll 2
        for (size_t i = 0; i < frames; ++i) {
            left->data[outputFrames + i] = source[2U * i];
            right->data[outputFrames + i] = source[2U * i + 1U];
        }

        _readPosition += frames * 2U;
        outputFrames += frames;

        if (_readPosition >= totalSamples) {
            releaseReadBuffer(static_cast<uint8_t>(index));
        }
    }

    // Nullen nur noch gezielt am Ende auffüllen, falls nicht genug PCM-Daten
    // für einen ganzen Block da waren.
    if (outputFrames < AUDIO_BLOCK_SAMPLES) {
        const size_t remainingBytes =
            (AUDIO_BLOCK_SAMPLES - outputFrames) * sizeof(int16_t);
        memset(&left->data[outputFrames], 0, remainingBytes);
        memset(&right->data[outputFrames], 0, remainingBytes);
    }

    transmit(left, 0);
    transmit(right, 1);

    release(left);
    release(right);

    _samplesPlayed += AUDIO_BLOCK_SAMPLES;

    bool finishNow = false;

    portENTER_CRITICAL(&_decoderMux);

    if (_decoderFinished && _readBuffer < 0 && _pcmState[0] != PCM_READY &&
        _pcmState[1] != PCM_READY) {
        finishNow = true;
    }

    _updateRunning = false;

    portEXIT_CRITICAL(&_decoderMux);

    if (finishNow) {
        /*
         * The decoder has stopped and all PCM has been consumed.
         * No more realtime update calls are necessary.
         */
        active = false;
        onPlaybackFinished();
    }
}
