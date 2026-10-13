#include "output_pdm.h"

#include <cmath>
#include <esp_err.h>
#include <esp_log.h>
#include "i2s_port.h"
#include "audio_graph_update_lock.h"

namespace {
constexpr char TAG[] = "AudioOutputPDM";

}

AudioOutputPDM::AudioOutputPDM() : AudioStream(2, inputQueueArray) {}
AudioOutputPDM::AudioOutputPDM(const Pins &pins)
    : AudioStream(2, inputQueueArray), pdmPins(pins), numChannels(pins.dataRight >= 0 ? 2U : 1U) {}
AudioOutputPDM::AudioOutputPDM(int8_t data) : AudioStream(2, inputQueueArray), pdmPins{data, -1} {}
AudioOutputPDM::~AudioOutputPDM() { end(); }

bool AudioOutputPDM::isRunning() const {
    return running.load(std::memory_order_acquire) != 0;
}

void AudioOutputPDM::setRunning(bool value) {
    running.store(value ? 1U : 0U, std::memory_order_release);
}

OSIZE
bool AudioOutputPDM::setChannelCount(uint channels) {
    if (channels < 1 || channels > 2) return false;
    audio_detail::GraphUpdateLock graphLock;
    if (isRunning()) return false;
    if (channels == 2 && pdmPins.dataRight < 0) return false;
    numChannels = channels;
    return true;
}

OSIZE
bool AudioOutputPDM::setSampleRate(float rate) {
    if (!std::isfinite(rate) || rate < 16000.0f || rate > 48000.0f) return false;

    audio_detail::GraphUpdateLock graphLock;

    auto discardQueuedPairs = [this]() {
        if (txQueue == nullptr) return;
        BlockPair pair;
        while (xQueueReceive(txQueue, &pair, 0) == pdTRUE) releasePair(pair);
    };

    // The graph lock prevents new queue entries. AudioI2SPort's shared writer
    // gate waits for in-flight PCM writes, and generation checks discard any
    // stale pair already dequeued before the rate change.
    const uint32_t oldGeneration = AudioI2SPort::currentSampleRateGeneration();
    const bool ok = AudioI2SPort::setSampleRate(rate);
    if (AudioI2SPort::currentSampleRateGeneration() != oldGeneration) {
        discardQueuedPairs();
    }
    return ok;
}

bool AudioOutputPDM::begin() {
    return beginInternal();
}

OSIZE
bool AudioOutputPDM::begin(const Pins &pins) {
    end();
    if (txHandle != nullptr || txQueue != nullptr) return false;
    pdmPins = pins;
    // Supplying two DATA pins is the explicit request for stereo mode.
    numChannels = pdmPins.dataRight >= 0 ? 2U : 1U;
    return beginInternal();
}

bool AudioOutputPDM::begin(int8_t data) {
    return begin(Pins{data, -1});
}

OSIZE
bool AudioOutputPDM::begin(int8_t dataLeft, int8_t dataRight) {
    if (dataRight < 0) return begin(dataLeft);
    return begin(Pins{dataLeft, dataRight});
}

OSIZE
bool AudioOutputPDM::beginHardware() {
    return beginInternal();
}

OSIZE
bool AudioOutputPDM::beginInternal() {
    audio_detail::GraphUpdateLock graphLock;
    if (isRunning()) return true;
    if (txHandle != nullptr || txQueue != nullptr) {
        ESP_LOGE(TAG, "Previous PDM output shutdown did not complete");
        return false;
    }
    if (pdmPins.dataLeft < 0 || (numChannels == 2 && pdmPins.dataRight < 0)) {
        ESP_LOGE(TAG, "Invalid PDM DATA pins: left=%d right=%d channels=%u",
                 pdmPins.dataLeft, pdmPins.dataRight, static_cast<unsigned>(numChannels));
        return false;
    }

    const float sampleRate = AudioStream::sampleRate();
    if (!std::isfinite(sampleRate) || sampleRate < 16000.0f || sampleRate > 48000.0f) {
        ESP_LOGE(TAG, "PDM PCM sample rate must be between 16 kHz and 48 kHz");
        return false;
    }

    txQueue = xQueueCreate(QUEUE_LENGTH, sizeof(BlockPair));
    if (txQueue == nullptr) return false;

    // In ESP-IDF's two-line DAC mode, DOUT emits the second (right) PCM slot
    // and DOUT2 emits the first (left) slot. Keep the public pin order intuitive.
    const int8_t i2sDout = numChannels == 2 ? pdmPins.dataRight : pdmPins.dataLeft;
    const int8_t i2sDout2 = numChannels == 2 ? pdmPins.dataLeft : -1;
    const AudioI2SPort::Pins portPins{-1, -1, i2sDout, -1, -1, CONTROLLER, i2sDout2};
    const AudioI2SPort::Format format{
        AudioI2SPort::PeripheralMode::PDM,
        AudioI2SPort::DataEncoding::PCM,
        static_cast<uint8_t>(numChannels),
        16,
        1
    };

    if (!AudioI2SPort::acquireTx(portPins, &txHandle, format)) {
        ESP_LOGE(TAG, "Could not acquire I2S0 PCM-to-PDM output");
        vQueueDelete(txQueue);
        txQueue = nullptr;
        txHandle = nullptr;
        return false;
    }

    if (!AudioI2SPort::setEventHandler(CONTROLLER, AudioI2SPort::Direction::Tx,
                                       &AudioOutputPDM::onI2STransmit, this)) {
        ESP_LOGE(TAG, "Could not register PDM TX callback");
        AudioI2SPort::release(CONTROLLER, AudioI2SPort::Direction::Tx);
        txHandle = nullptr;
        vQueueDelete(txQueue);
        txQueue = nullptr;
        return false;
    }

    setRunning(true);

    TaskHandle_t createdTask = nullptr;
    const BaseType_t result = xTaskCreatePinnedToCore(
        txTaskEntry, "AudioPDM", 3072, this, configMAX_PRIORITIES - 2,
        &createdTask, AUDIO_PROCESSING_CORE);
    if (result == pdPASS) txTask.store(createdTask, std::memory_order_release);

    if (result != pdPASS) {
        setRunning(false);
        txTask.store(nullptr, std::memory_order_release);
        AudioI2SPort::clearEventHandler(CONTROLLER, AudioI2SPort::Direction::Tx, this);
        AudioI2SPort::release(CONTROLLER, AudioI2SPort::Direction::Tx);
        txHandle = nullptr;
        vQueueDelete(txQueue);
        txQueue = nullptr;
        return false;
    }

    if (!AudioI2SPort::start(CONTROLLER, AudioI2SPort::Direction::Tx)) {
        setRunning(false);
        // Wait for the finite queue/write timeout to expire before freeing
        // resources referenced by the TX task. Avoid a notification racing
        // the task's self-delete path.
        while (txTask.load(std::memory_order_acquire) != nullptr) vTaskDelay(1);

        AudioI2SPort::clearEventHandler(CONTROLLER, AudioI2SPort::Direction::Tx, this);
        if (!AudioI2SPort::stop(CONTROLLER, AudioI2SPort::Direction::Tx)) {
            ESP_LOGE(TAG, "PDM TX channel could not be stopped after startup failure; retaining port resources");
            return false;
        }
        AudioI2SPort::release(CONTROLLER, AudioI2SPort::Direction::Tx);
        txHandle = nullptr;
        if (txQueue != nullptr) {
            vQueueDelete(txQueue);
            txQueue = nullptr;
        }
        return false;
    }

    return true;
}

OSIZE
void AudioOutputPDM::end() {
    audio_detail::GraphUpdateLock graphLock;
    if (!isRunning() && txQueue == nullptr && txHandle == nullptr &&
        txTask.load(std::memory_order_acquire) == nullptr) return;

    setRunning(false);

    if (txTask.load(std::memory_order_acquire) != nullptr) {
        // The writer has finite queue/write timeouts. Wait for its explicit exit
        // signal before deleting the queue or mutex it may still access.
        while (txTask.load(std::memory_order_acquire) != nullptr) vTaskDelay(1);
    }

    if (txQueue != nullptr) {
        BlockPair pair;
        while (xQueueReceive(txQueue, &pair, 0) == pdTRUE) releasePair(pair);
    }

    if (txHandle != nullptr) {
        AudioI2SPort::clearEventHandler(CONTROLLER, AudioI2SPort::Direction::Tx, this);
        if (!AudioI2SPort::stop(CONTROLLER, AudioI2SPort::Direction::Tx)) {
            ESP_LOGE(TAG, "PDM TX channel could not be stopped; retaining port resources for retry");
            return;
        }
        AudioI2SPort::release(CONTROLLER, AudioI2SPort::Direction::Tx);
        txHandle = nullptr;
    }

    if (txQueue != nullptr) {
        vQueueDelete(txQueue);
        txQueue = nullptr;
    }
    if (txWriteMutex != nullptr) {
    }
}

OSPEED
void AudioOutputPDM::update() {
    BlockPair pair;
    pair.left = receiveReadOnly(0);
    pair.right = receiveReadOnly(1);

    // Match AudioOutputI2S: don't enqueue a pair when neither input delivered
    // a block. The enabled I2S/PDM DMA keeps running and auto-clears DMA buffers
    // to silence; avoiding empty queue entries prevents them delaying real audio.
    if (pair.left == nullptr && pair.right == nullptr) return;
    pair.rateGeneration = AudioI2SPort::currentSampleRateGeneration();

    if (txQueue == nullptr) {
        releasePair(pair);
        return;
    }

    if (xQueueSend(txQueue, &pair, 0) != pdTRUE) {
        BlockPair oldPair;
        if (xQueueReceive(txQueue, &oldPair, 0) == pdTRUE) {
            releasePair(oldPair);
        }
        if (xQueueSend(txQueue, &pair, 0) != pdTRUE) {
            releasePair(pair);
        }
    }
}

OSPEED
void AudioOutputPDM::releasePair(BlockPair &pair) {
    if (pair.left != nullptr) {
        release(pair.left);
        pair.left = nullptr;
    }
    if (pair.right != nullptr) {
        release(pair.right);
        pair.right = nullptr;
    }
}

OSPEED
void AudioOutputPDM::txTaskEntry(void *arg) {
    static_cast<AudioOutputPDM *>(arg)->txTaskLoop();
}

OSPEED
bool AudioOutputPDM::onI2STransmit(i2s_chan_handle_t, i2s_event_data_t *, void *userContext) {
    auto *output = static_cast<AudioOutputPDM *>(userContext);
    if (output == nullptr || output->running.load(std::memory_order_acquire) == 0) return false;
    if (!AudioI2SPort::isClockOwner(CONTROLLER, AudioI2SPort::Direction::Tx)) return false;
    return AudioStream::update_all_from_isr();
}

OSPEED
void AudioOutputPDM::txTaskLoop() {
    uint32_t writeErrorCount = 0;
    uint32_t lastErrorLogMs = 0;

    // PDM TX accepts 16-bit PCM data. Stereo is interleaved L,R; mono has
    // one word per PCM frame. Hardware creates the high-rate PDM bitstream.
    int16_t pcmBuffer[AUDIO_BLOCK_SAMPLES * 2U];
    BlockPair pair{nullptr, nullptr, 0};

    while (isRunning()) {
        if (!isRunning() ||
            xQueueReceive(txQueue, &pair, pdMS_TO_TICKS(10)) != pdTRUE) {
            if (!isRunning()) break;
            continue;
        }

        // Discard blocks that were queued before a global rate change.
        if (!isRunning() ||
            pair.rateGeneration != AudioI2SPort::currentSampleRateGeneration()) {
            releasePair(pair);
            pair = {nullptr, nullptr, 0};
            if (!isRunning()) break;
            continue;
        }

        const int16_t *left = pair.left ? pair.left->data : nullptr;
        const int16_t *right = pair.right ? pair.right->data : nullptr;
        size_t sampleCount = AUDIO_BLOCK_SAMPLES;

        if (numChannels == 1) {
            for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
                int32_t mono;
                if (left && right) {
                    mono = (static_cast<int32_t>(left[i]) + static_cast<int32_t>(right[i])) / 2;
                } else if (left) {
                    mono = left[i];
                } else if (right) {
                    mono = right[i];
                } else {
                    mono = 0;
                }
                pcmBuffer[i] = static_cast<int16_t>(mono);
            }
        } else {
            sampleCount *= 2U;
            for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
                pcmBuffer[i * 2U] = left ? left[i] : 0;
                pcmBuffer[i * 2U + 1U] = right ? right[i] : 0;
            }
        }

        size_t bytesWritten = 0;
        const size_t bytesToWrite = sampleCount * sizeof(int16_t);
        if (!AudioI2SPort::lockForWrite(pdMS_TO_TICKS(500))) {
            releasePair(pair);
            pair = {nullptr, nullptr, 0};
            continue;
        }
        if (!isRunning() ||
            pair.rateGeneration != AudioI2SPort::currentSampleRateGeneration()) {
            releasePair(pair);
            pair = {nullptr, nullptr, 0};
            AudioI2SPort::unlockAfterWrite();
            if (!isRunning()) break;
            continue;
        }

        const esp_err_t err = i2s_channel_write(txHandle, pcmBuffer, bytesToWrite, &bytesWritten, 100);

        if (err != ESP_OK || bytesWritten != bytesToWrite) {
            const uint32_t now = millis();
            ++writeErrorCount;
            if (lastErrorLogMs == 0 || now - lastErrorLogMs >= 1000) {
                ESP_LOGE(TAG, "PDM TX error #%lu: %s, bytes=%lu/%lu",
                         static_cast<unsigned long>(writeErrorCount), esp_err_to_name(err),
                         static_cast<unsigned long>(bytesWritten), static_cast<unsigned long>(bytesToWrite));
                lastErrorLogMs = now;
            }
        }

        AudioI2SPort::unlockAfterWrite();
        releasePair(pair);
        pair = {nullptr, nullptr, 0};
    }

    while (xQueueReceive(txQueue, &pair, 0) == pdTRUE) releasePair(pair);
    txTask.store(nullptr, std::memory_order_release);
    vTaskDelete(nullptr);
}
