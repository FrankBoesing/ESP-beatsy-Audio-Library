#include "output_pdm.h"
#include <esp_memory_utils.h>
#include <esp_cache.h>
#include <cstring>

#include <cmath>
#include <esp_err.h>
#include <esp_log.h>
#include "i2s_port.h"


static constexpr char TAG[] = "AudioOutputPDM";

OSIZE
AudioOutputPDM::AudioOutputPDM() : AudioStream(2, inputQueueArray) {}
OSIZE
AudioOutputPDM::AudioOutputPDM(const Pins &pins)
    : AudioStream(2, inputQueueArray), pdmPins(pins), numChannels(pins.dataRight >= 0 ? 2U : 1U) {}
OSIZE
AudioOutputPDM::AudioOutputPDM(int8_t data) : AudioStream(2, inputQueueArray), pdmPins{data, -1} {}
OSIZE
AudioOutputPDM::~AudioOutputPDM() { end(); }

OSPEED
bool AudioOutputPDM::isRunning() const {
    return running.load(std::memory_order_acquire) != 0;
}

OSIZE
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

    return AudioI2SPort::setSampleRate(rate);
}

OSIZE
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

OSIZE
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
#if defined(CONFIG_I2S_ISR_IRAM_SAFE) && CONFIG_I2S_ISR_IRAM_SAFE
    if (!esp_ptr_internal(this)) {
        ESP_LOGE(TAG, "IRAM-safe I2S callbacks require AudioOutputPDM in internal RAM");
        return false;
    }
#endif
    if (txHandle != nullptr) {
        ESP_LOGE(TAG, "Previous PDM output shutdown did not complete");
        return false;
    }
    if (pdmPins.dataLeft < 0 || (numChannels == 2 && pdmPins.dataRight < 0)) {
        ESP_LOGE(TAG, "Invalid PDM DATA pins: left=%d right=%d channels=%u",
                 pdmPins.dataLeft, pdmPins.dataRight, (unsigned)(numChannels));
        return false;
    }

    const float sampleRate = AudioStream::sampleRate();
    if (!std::isfinite(sampleRate) || sampleRate < 16000.0f || sampleRate > 48000.0f) {
        ESP_LOGE(TAG, "PDM PCM sample rate must be between 16 kHz and 48 kHz");
        return false;
    }

    // In ESP-IDF's two-line DAC mode, DOUT emits the second (right) PCM slot
    // and DOUT2 emits the first (left) slot. Keep the public pin order intuitive.
    const int8_t i2sDout = numChannels == 2 ? pdmPins.dataRight : pdmPins.dataLeft;
    const int8_t i2sDout2 = numChannels == 2 ? pdmPins.dataLeft : -1;
    const AudioI2SPort::Pins portPins{-1, -1, i2sDout, -1, -1, CONTROLLER, i2sDout2};
    const AudioI2SPort::Format format{
        AudioI2SPort::PeripheralMode::PDM,
        AudioI2SPort::DataEncoding::PCM,
        (uint8_t)(numChannels),
        16,
        1
    };

    if (!AudioI2SPort::acquireTx(portPins, &txHandle, format)) {
        ESP_LOGE(TAG, "Could not acquire I2S0 PCM-to-PDM output");
        txHandle = nullptr;
        return false;
    }

    if (!AudioI2SPort::setEventHandler(CONTROLLER, AudioI2SPort::Direction::Tx,
                                       &AudioOutputPDM::onI2STransmit, this)) {
        ESP_LOGE(TAG, "Could not register PDM TX callback");
        if (!AudioI2SPort::release(CONTROLLER, AudioI2SPort::Direction::Tx)) {
            ESP_LOGE(TAG, "Could not release PDM port after callback registration failure; retaining handle");
            return false;
        }
        txHandle = nullptr;
        return false;
    }

    pendingDmaBuffer.store(nullptr, std::memory_order_relaxed);
    setRunning(true);

    if (!AudioI2SPort::start(CONTROLLER, AudioI2SPort::Direction::Tx)) {
        setRunning(false);
        AudioI2SPort::clearEventHandler(CONTROLLER, AudioI2SPort::Direction::Tx, this);
        pendingDmaBuffer.store(nullptr, std::memory_order_relaxed);
        if (!AudioI2SPort::stop(CONTROLLER, AudioI2SPort::Direction::Tx)) {
            ESP_LOGE(TAG, "PDM TX channel could not be stopped after startup failure; retaining port resources");
            return false;
        }
        if (!AudioI2SPort::release(CONTROLLER, AudioI2SPort::Direction::Tx)) {
            ESP_LOGE(TAG, "Could not release PDM port after startup failure; retaining resources");
            return false;
        }
        txHandle = nullptr;
        return false;
    }

    return true;
}


OSIZE
void AudioOutputPDM::end() {
    audio_detail::GraphUpdateLock graphLock;
    if (!isRunning() && txQueueCount == 0 && txHandle == nullptr) return;

    setRunning(false);

    BlockPair pair;
    while (dequeuePair(pair)) releasePair(pair);

    if (txHandle != nullptr) {
        AudioI2SPort::clearEventHandler(CONTROLLER, AudioI2SPort::Direction::Tx, this);
        pendingDmaBuffer.store(nullptr, std::memory_order_relaxed);
        if (!AudioI2SPort::stop(CONTROLLER, AudioI2SPort::Direction::Tx)) {
            ESP_LOGE(TAG, "PDM TX channel could not be stopped; retaining port resources for retry");
            return;
        }
        if (!AudioI2SPort::release(CONTROLLER, AudioI2SPort::Direction::Tx)) {
            ESP_LOGE(TAG, "Could not release PDM port; retaining resources for retry");
            return;
        }
        txHandle = nullptr;
    }
}


OSPEED
void AudioOutputPDM::update() {
    BlockPair pair{receiveReadOnly(0), receiveReadOnly(1)};

    if (pair.left != nullptr || pair.right != nullptr) {
        if (!isRunning()) {
            releasePair(pair);
        } else if (!enqueuePair(pair)) {
            BlockPair oldPair;
            if (dequeuePair(oldPair)) releasePair(oldPair);
            if (!enqueuePair(pair)) releasePair(pair);
        }
    }

    // The TX callback publishes the completed PDM DMA buffer.
    int16_t *dmaBuffer = pendingDmaBuffer.exchange(nullptr, std::memory_order_acq_rel);
    if (dmaBuffer == nullptr || !isRunning()) return;

    const size_t sampleCount = AUDIO_BLOCK_SAMPLES * numChannels;
    const size_t bufferBytes = sampleCount * sizeof(int16_t);
    BlockPair ready{nullptr, nullptr};
    if (!dequeuePair(ready)) {
        // Underrun: clear only the DMA buffer that will be reused.
        std::memset(dmaBuffer, 0, bufferBytes);
        if (esp_cache_get_line_size_by_addr(dmaBuffer) > 0) {
            (void)esp_cache_msync(dmaBuffer, bufferBytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        }
        return;
    }

    const int16_t *left = ready.left ? ready.left->data : nullptr;
    const int16_t *right = ready.right ? ready.right->data : nullptr;

    if (numChannels == 1) {
        GCC_UNROLL
        for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            int32_t mono;
            if (left && right) {
                mono = ((int32_t)left[i] + (int32_t)right[i]) / 2;
            } else if (left) {
                mono = left[i];
            } else if (right) {
                mono = right[i];
            } else {
                mono = 0;
            }
            dmaBuffer[i] = (int16_t)mono;
        }
    } else {
        GCC_UNROLL
        for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            dmaBuffer[i * 2U] = left ? left[i] : 0;
            dmaBuffer[i * 2U + 1U] = right ? right[i] : 0;
        }
    }

    if (esp_cache_get_line_size_by_addr(dmaBuffer) > 0) {
        (void)esp_cache_msync(dmaBuffer, bufferBytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }

    releasePair(ready);
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
bool AudioOutputPDM::enqueuePair(const BlockPair &pair) {
    if (txQueueCount == QUEUE_LENGTH) return false;

    txQueue[txQueueTail] = pair;
    txQueueTail = (txQueueTail + 1U) % QUEUE_LENGTH;
    ++txQueueCount;
    return true;
}

OSPEED
bool AudioOutputPDM::dequeuePair(BlockPair &pair) {
    if (txQueueCount == 0) return false;

    pair = txQueue[txQueueHead];
    txQueue[txQueueHead] = {nullptr, nullptr};
    txQueueHead = (txQueueHead + 1U) % QUEUE_LENGTH;
    --txQueueCount;
    return true;
}

OSPEED
bool AUDIO_I2S_ISR_ATTR AudioOutputPDM::onI2STransmit(i2s_chan_handle_t, i2s_event_data_t *event, void *userContext) {
    auto *output = static_cast<AudioOutputPDM *>(userContext);
    if (output == nullptr || output->running.load(std::memory_order_acquire) == 0) return false;

    int16_t *dmaBuffer = nullptr;
    const size_t expectedBytes = sizeof(int16_t) * AUDIO_BLOCK_SAMPLES * output->numChannels;
    if (event != nullptr && event->dma_buf != nullptr && event->size == expectedBytes) {
        dmaBuffer = static_cast<int16_t *>(event->dma_buf);
    }
    output->pendingDmaBuffer.store(dmaBuffer, std::memory_order_release);

    if (!AudioI2SPort::isClockOwner(CONTROLLER, AudioI2SPort::Direction::Tx)) return false;
    return AudioStream::update_all_from_isr();
}
