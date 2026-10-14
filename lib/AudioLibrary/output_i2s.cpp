#include "output_i2s.h"
#include <esp_memory_utils.h>
#include <esp_cache.h>
#include <esp_private/esp_cache_private.h>
#include <cstring>

#include <esp_err.h>
#include <cmath>
#include "i2s_port.h"

static constexpr char TAG[] = "I2S";

OSIZE
AudioOutputI2S::AudioOutputI2S() : AudioStream(2, inputQueueArray) {} // kein Hardware-Init hier
OSIZE
AudioOutputI2S::AudioOutputI2S(const Pins &pins)
    : AudioStream(2, inputQueueArray), i2sPins(pins) {} // kein Hardware-Init hier
OSIZE
AudioOutputI2S::~AudioOutputI2S() { end(); }
OSPEED
bool AudioOutputI2S::isRunning() const { return running.load(std::memory_order_acquire) != 0; }
OSIZE
void AudioOutputI2S::setRunning(bool value) { running.store(value ? 1U : 0U, std::memory_order_release); }

OSIZE
bool AudioOutputI2S::begin() { return beginInternal(); }
OSIZE
bool AudioOutputI2S::begin(const Pins &pins) {
    end();
    if (txHandle != nullptr) return false;
    i2sPins = pins;
    return beginInternal();
}
OSIZE
bool AudioOutputI2S::beginHardware() { return beginInternal(); }

OSIZE
bool AudioOutputI2S::setChannelCount(uint num) {
    if (num < 1 || num > 2) return false;
    audio_detail::GraphUpdateLock graphLock;
    if (isRunning()) return false;
    numChannels = num;
    return true;
}

OSIZE
bool AudioOutputI2S::setSampleRate(float hz) {
    if (!std::isfinite(hz) || hz <= 0.0f) return false;
    return AudioI2SPort::setSampleRate(hz);
}

OSIZE
bool AudioOutputI2S::beginInternal() {
    audio_detail::GraphUpdateLock graphLock;
    if (isRunning()) return true;
#if defined(CONFIG_I2S_ISR_IRAM_SAFE) && CONFIG_I2S_ISR_IRAM_SAFE
    if (!esp_ptr_internal(this)) {
        ESP_LOGE(TAG, "IRAM-safe I2S callbacks require AudioOutputI2S in internal RAM");
        return false;
    }
#endif
    if (txHandle != nullptr) {
        ESP_LOGE(TAG, "Previous I2S output shutdown did not complete");
        return false;
    }
    if (i2sPins.bclk < 0 || i2sPins.ws < 0 || i2sPins.dout < 0) {
        ESP_LOGE(TAG, "Invalid I2S output pins");
        return false;
    }

    const AudioI2SPort::Pins portPins{i2sPins.bclk, i2sPins.ws, i2sPins.dout, -1, i2sPins.mclk, i2sPins.controller};
    if (!AudioI2SPort::acquireTx(portPins, &txHandle)) {
        txHandle = nullptr;
        return false;
    }

    if (!AudioI2SPort::setEventHandler(i2sPins.controller, AudioI2SPort::Direction::Tx, &AudioOutputI2S::onI2STransmit,
                                       this)) {
        ESP_LOGE(TAG, "Could not register TX event handler");
        if (!AudioI2SPort::release(i2sPins.controller, AudioI2SPort::Direction::Tx)) {
            ESP_LOGE(TAG, "Could not release TX port after callback registration failure; retaining handle");
            return false;
        }
        txHandle = nullptr;
        return false;
    }

    pendingDmaBuffer.store(nullptr, std::memory_order_relaxed);
    setRunning(true);

    if (!AudioI2SPort::start(i2sPins.controller, AudioI2SPort::Direction::Tx)) {
        setRunning(false);
        AudioI2SPort::clearEventHandler(i2sPins.controller, AudioI2SPort::Direction::Tx, this);
        pendingDmaBuffer.store(nullptr, std::memory_order_relaxed);
        if (!AudioI2SPort::stop(i2sPins.controller, AudioI2SPort::Direction::Tx)) {
            ESP_LOGE(TAG, "TX channel could not be stopped after startup failure; retaining port resources");
            return false;
        }
        if (!AudioI2SPort::release(i2sPins.controller, AudioI2SPort::Direction::Tx)) {
            ESP_LOGE(TAG, "Could not release TX port after startup failure; retaining resources");
            return false;
        }
        txHandle = nullptr;
        return false;
    }

    return true;
}

OSIZE
void AudioOutputI2S::end() {
    audio_detail::GraphUpdateLock graphLock;
    if (!isRunning() && txQueueCount == 0 && txHandle == nullptr) return;

    setRunning(false);

    BlockPair pair;
    while (dequeuePair(pair)) releasePair(pair);

    if (txHandle != nullptr) {
        AudioI2SPort::clearEventHandler(i2sPins.controller, AudioI2SPort::Direction::Tx, this);
        pendingDmaBuffer.store(nullptr, std::memory_order_relaxed);
        if (!AudioI2SPort::stop(i2sPins.controller, AudioI2SPort::Direction::Tx)) {
            ESP_LOGE(TAG, "TX channel could not be stopped; retaining port resources for retry");
            return;
        }
        if (!AudioI2SPort::release(i2sPins.controller, AudioI2SPort::Direction::Tx)) {
            ESP_LOGE(TAG, "Could not release TX port; retaining resources for retry");
            return;
        }
        txHandle = nullptr;
    }

}

OSPEED
void AudioOutputI2S::update() {
    BlockPair pair{receiveReadOnly(0), receiveReadOnly(1)};

    if (pair.left != nullptr || pair.right != nullptr) {
        if (!isRunning()) {
            releasePair(pair);
        } else if (!enqueuePair(pair)) {
            BlockPair oldPair;

            // Keep the newest audio if the output falls behind.
            if (dequeuePair(oldPair)) releasePair(oldPair);

            if (!enqueuePair(pair)) releasePair(pair);
        }
    }

    // The callback publishes the DMA buffer that just finished transmitting.
    // If multiple callbacks arrive before this update, the latest buffer wins;
    // older buffers remain silent rather than risking writes to a reused buffer.
    uint32_t *dmaBuffer = pendingDmaBuffer.exchange(nullptr, std::memory_order_acq_rel);
    if (dmaBuffer == nullptr || !isRunning()) {
        return;
    }

    BlockPair ready{nullptr, nullptr};
    if (!dequeuePair(ready)) {
        // No PCM is available: clear only this DMA buffer instead of clearing every callback.
        std::memset(dmaBuffer, 0, sizeof(uint32_t) * AUDIO_BLOCK_SAMPLES);

        if (esp_cache_get_line_size_by_addr(dmaBuffer) > 0) {
            (void)esp_cache_msync(dmaBuffer, sizeof(uint32_t) * AUDIO_BLOCK_SAMPLES,
                                  ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        }
        return;
    }

    if (numChannels == 1) {
        const int16_t *__restrict left = ready.left ? ready.left->data : nullptr;
        const int16_t *__restrict right = ready.right ? ready.right->data : nullptr;
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

            const uint32_t sample = (uint16_t)((int16_t)(mono));
            dmaBuffer[i] = sample | (sample << 16);
        }
    } else {
        const int16_t *left = ready.left ? ready.left->data : audio_zeroblock;
        const int16_t *right = ready.right ? ready.right->data : audio_zeroblock;

        GCC_UNROLL
        for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            const uint32_t l = (uint16_t)(left[i]);
            const uint32_t r = (uint16_t)(right[i]);
            dmaBuffer[i] = l | (r << 16);
        }
    }

    // Match ESP-IDF: synchronize only when this DMA buffer is cacheable.
    if (esp_cache_get_line_size_by_addr(dmaBuffer) > 0) {
        (void)esp_cache_msync(dmaBuffer, sizeof(uint32_t) * AUDIO_BLOCK_SAMPLES,
                              ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }

    releasePair(ready);
}

OSPEED
void AudioOutputI2S::releasePair(BlockPair &pair) {
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
bool AudioOutputI2S::enqueuePair(const BlockPair &pair) {
    if (txQueueCount == QUEUE_LENGTH) return false;

    txQueue[txQueueTail] = pair;
    txQueueTail = (txQueueTail + 1U) % QUEUE_LENGTH;
    ++txQueueCount;
    return true;
}

OSPEED
bool AudioOutputI2S::dequeuePair(BlockPair &pair) {
    if (txQueueCount == 0) return false;

    pair = txQueue[txQueueHead];
    txQueue[txQueueHead] = {nullptr, nullptr};
    txQueueHead = (txQueueHead + 1U) % QUEUE_LENGTH;
    --txQueueCount;
    return true;
}

OSPEED
bool AUDIO_I2S_ISR_ATTR AudioOutputI2S::onI2STransmit(i2s_chan_handle_t, i2s_event_data_t *event, void *userContext) {
    auto *output = static_cast<AudioOutputI2S *>(userContext);
    if (output == nullptr || output->running.load(std::memory_order_acquire) == 0) return false;

    uint32_t *dmaBuffer = nullptr;
    if (event != nullptr && event->dma_buf != nullptr &&
        event->size == sizeof(uint32_t) * AUDIO_BLOCK_SAMPLES) {
        dmaBuffer = static_cast<uint32_t *>(event->dma_buf);
    }
    output->pendingDmaBuffer.store(dmaBuffer, std::memory_order_release);

    if (!AudioI2SPort::isClockOwner(output->i2sPins.controller, AudioI2SPort::Direction::Tx)) return false;
    return AudioStream::update_all_from_isr();
}
