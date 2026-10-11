#include "output_pdm.h"

#include <cmath>
#include <cstring>
#include <esp_err.h>
#include <esp_log.h>
#include "i2s_port.h"

namespace {
constexpr char TAG[] = "AudioOutputPDM";
}

AudioOutputPDM::AudioOutputPDM() : AudioStream(2, inputQueueArray) {}
AudioOutputPDM::AudioOutputPDM(const Pins &pins) : AudioStream(2, inputQueueArray), pdmPins(pins) {}
AudioOutputPDM::AudioOutputPDM(int8_t data) : AudioStream(2, inputQueueArray), pdmPins{data, -1} {}
AudioOutputPDM::~AudioOutputPDM() { end(); }

bool AudioOutputPDM::isRunning() const {
    return running.load(std::memory_order_acquire) != 0;
}

uint32_t AudioOutputPDM::txCallbackCount() const {
    return txCallbackCountValue.load(std::memory_order_relaxed);
}

void AudioOutputPDM::setRunning(bool value) {
    running.store(value ? 1U : 0U, std::memory_order_release);
}

void AudioOutputPDM::resetTxCallbackCount() {
    txCallbackCountValue.store(0, std::memory_order_relaxed);
}

bool AudioOutputPDM::setChannelCount(uint channels) {
    if (isRunning() || channels < 1 || channels > 2) return false;
    if (channels == 2 && pdmPins.data2 < 0) return false;
    numChannels = channels;
    return true;
}

bool AudioOutputPDM::setSampleRate(float rate) {
    if (!std::isfinite(rate) || rate < 16000.0f || rate > 48000.0f) return false;

    if (txQueue != nullptr) {
        BlockPair pair;
        while (xQueueReceive(txQueue, &pair, 0) == pdTRUE) releasePair(pair);
    }

    return AudioI2SPort::setSampleRate(rate);
}

bool AudioOutputPDM::begin() {
    return beginInternal();
}

bool AudioOutputPDM::begin(const Pins &pins) {
    end();
    pdmPins = pins;
    if (numChannels == 2 && pdmPins.data2 < 0) return false;
    return beginInternal();
}

bool AudioOutputPDM::begin(int8_t data) {
    return begin(Pins{data, -1});
}

bool AudioOutputPDM::beginHardware() {
    return beginInternal();
}

bool AudioOutputPDM::beginInternal() {
    if (isRunning()) return true;
    if (pdmPins.data < 0 || (numChannels == 2 && pdmPins.data2 < 0)) {
        ESP_LOGE(TAG, "Invalid PDM DATA pins: data=%d data2=%d channels=%u",
                 pdmPins.data, pdmPins.data2, static_cast<unsigned>(numChannels));
        return false;
    }

    const float sampleRate = AudioStream::sampleRate();
    if (!std::isfinite(sampleRate) || sampleRate < 16000.0f || sampleRate > 48000.0f) {
        ESP_LOGE(TAG, "PDM PCM sample rate must be between 16 kHz and 48 kHz");
        return false;
    }

    txQueue = xQueueCreate(QUEUE_LENGTH, sizeof(BlockPair));
    if (txQueue == nullptr) return false;

    const AudioI2SPort::Pins portPins{-1, -1, pdmPins.data, -1, -1, CONTROLLER, pdmPins.data2};
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

    resetTxCallbackCount();
    setRunning(true);

    const BaseType_t result = xTaskCreatePinnedToCore(
        txTaskEntry, "AudioPDM", 3072, this, configMAX_PRIORITIES - 2,
        &txTask, AUDIO_PROCESSING_CORE);

    if (result != pdPASS) {
        setRunning(false);
        txTask = nullptr;
        AudioI2SPort::clearEventHandler(CONTROLLER, AudioI2SPort::Direction::Tx, this);
        AudioI2SPort::release(CONTROLLER, AudioI2SPort::Direction::Tx);
        txHandle = nullptr;
        vQueueDelete(txQueue);
        txQueue = nullptr;
        return false;
    }

    if (!AudioI2SPort::start(CONTROLLER, AudioI2SPort::Direction::Tx)) {
        setRunning(false);
        xTaskNotifyGive(txTask);
        const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(200);
        while (txTask != nullptr && static_cast<int32_t>(deadline - xTaskGetTickCount()) > 0) vTaskDelay(1);

        AudioI2SPort::clearEventHandler(CONTROLLER, AudioI2SPort::Direction::Tx, this);
        AudioI2SPort::stop(CONTROLLER, AudioI2SPort::Direction::Tx);
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

void AudioOutputPDM::end() {
    if (!isRunning() && txQueue == nullptr && txHandle == nullptr) return;

    setRunning(false);

    if (txTask != nullptr) {
        xTaskNotifyGive(txTask);
        const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(200);
        while (txTask != nullptr && static_cast<int32_t>(deadline - xTaskGetTickCount()) > 0) vTaskDelay(1);
    }

    if (txQueue != nullptr) {
        BlockPair pair;
        while (xQueueReceive(txQueue, &pair, 0) == pdTRUE) releasePair(pair);
    }

    if (txHandle != nullptr) {
        AudioI2SPort::clearEventHandler(CONTROLLER, AudioI2SPort::Direction::Tx, this);
        AudioI2SPort::stop(CONTROLLER, AudioI2SPort::Direction::Tx);
        AudioI2SPort::release(CONTROLLER, AudioI2SPort::Direction::Tx);
        txHandle = nullptr;
    }

    if (txQueue != nullptr) {
        vQueueDelete(txQueue);
        txQueue = nullptr;
    }
}

void AudioOutputPDM::update() {
    BlockPair pair;
    pair.left = receiveReadOnly(0);
    pair.right = receiveReadOnly(1);

    // Also queue zero PCM when no source is connected. This keeps a stable
    // PDM stream during silence instead of stopping DMA submissions.
    if (txQueue == nullptr) {
        releasePair(pair);
        return;
    }

    if (xQueueSend(txQueue, &pair, 0) != pdTRUE) {
        BlockPair oldPair;
        if (xQueueReceive(txQueue, &oldPair, 0) == pdTRUE) releasePair(oldPair);
        if (xQueueSend(txQueue, &pair, 0) != pdTRUE) releasePair(pair);
    }
}

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

void AudioOutputPDM::txTaskEntry(void *arg) {
    static_cast<AudioOutputPDM *>(arg)->txTaskLoop();
}

bool AudioOutputPDM::onI2STransmit(i2s_chan_handle_t, i2s_event_data_t *, void *userContext) {
    auto *output = static_cast<AudioOutputPDM *>(userContext);
    if (output == nullptr || output->running.load(std::memory_order_acquire) == 0) return false;
    output->txCallbackCountValue.fetch_add(1, std::memory_order_relaxed);
    if (!AudioI2SPort::isClockOwner(CONTROLLER, AudioI2SPort::Direction::Tx)) return false;
    return AudioStream::update_all_from_isr();
}

void AudioOutputPDM::txTaskLoop() {
    static uint32_t writeErrorCount = 0;
    static uint32_t lastErrorLogMs = 0;

    // PDM TX accepts 16-bit PCM data. Stereo is interleaved L,R; mono has
    // one word per PCM frame. Hardware creates the high-rate PDM bitstream.
    int16_t pcmBuffer[AUDIO_BLOCK_SAMPLES * 2U];
    BlockPair pair{nullptr, nullptr};

    while (isRunning()) {
        if (xQueueReceive(txQueue, &pair, pdMS_TO_TICKS(100)) != pdTRUE) continue;

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

        releasePair(pair);
        pair = {nullptr, nullptr};
    }

    while (xQueueReceive(txQueue, &pair, 0) == pdTRUE) releasePair(pair);
    txTask = nullptr;
    vTaskDelete(nullptr);
}
