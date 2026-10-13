#include "input_i2s.h"
#include <esp_memory_utils.h>

#include <esp_log.h>
#include "i2s_port.h"

namespace {
constexpr char TAG[] = "AudioInputI2S";
} // namespace



OSIZE
AudioInputI2S::AudioInputI2S() : AudioStream(0, nullptr) {}
OSIZE
AudioInputI2S::AudioInputI2S(const Pins &pins) : AudioStream(0, nullptr), i2sPins(pins) {}
OSIZE
AudioInputI2S::~AudioInputI2S() { end(); }
OSIZE
bool AudioInputI2S::isRunning() const { return running.load(std::memory_order_acquire) != 0; }
OSIZE
uint32_t AudioInputI2S::rxBlockCount() const { return rxBlockCountValue.load(std::memory_order_relaxed); }
OSIZE
uint32_t AudioInputI2S::rxErrorCount() const { return rxErrorCountValue.load(std::memory_order_relaxed); }
OSIZE
uint32_t AudioInputI2S::droppedBlockCount() const { return droppedBlockCountValue.load(std::memory_order_relaxed); }

OSIZE
bool AudioInputI2S::begin() { return beginInternal(); }
OSIZE
bool AudioInputI2S::begin(const Pins &pins) {
    end();
    if (rxHandle != nullptr) return false; // Previous shutdown did not complete.
    i2sPins = pins;
    return beginInternal();
}
OSIZE
bool AudioInputI2S::beginHardware() { return beginInternal(); }

OSIZE
bool AudioInputI2S::beginInternal() {
    audio_detail::GraphUpdateLock graphLock;
    if (isRunning()) return true;
#if defined(CONFIG_I2S_ISR_IRAM_SAFE) && CONFIG_I2S_ISR_IRAM_SAFE
    if (!esp_ptr_internal(this)) {
        ESP_LOGE(TAG, "IRAM-safe I2S callbacks require AudioInputI2S in internal RAM");
        return false;
    }
#endif
    if (rxHandle != nullptr) {
        ESP_LOGE(TAG, "Previous I2S input shutdown did not complete");
        return false;
    }

    if (i2sPins.bclk < 0 || i2sPins.ws < 0 || i2sPins.din < 0) {
        ESP_LOGE(TAG, "Invalid I2S pins: BCLK=%d WS=%d DIN=%d", i2sPins.bclk, i2sPins.ws, i2sPins.din);
        return false;
    }

    const AudioI2SPort::Pins portPins{i2sPins.bclk, i2sPins.ws, -1, i2sPins.din, i2sPins.mclk, i2sPins.controller};

    if (!AudioI2SPort::acquireRx(portPins, &rxHandle)) {
        rxHandle = nullptr;
        return false;
    }

    if (!AudioI2SPort::setEventHandler(i2sPins.controller, AudioI2SPort::Direction::Rx, &AudioInputI2S::onI2SReceive,
                                       this)) {
        ESP_LOGE(TAG, "Could not register RX event handler");
        if (!AudioI2SPort::release(i2sPins.controller, AudioI2SPort::Direction::Rx)) {
            ESP_LOGE(TAG, "Could not release RX port after callback registration failure; retaining handle");
            return false;
        }
        rxHandle = nullptr;
        return false;
    }

    writeBlockIndex.store(0, std::memory_order_relaxed);
    readBlockIndex.store(0, std::memory_order_relaxed);
    rxBlockCountValue.store(0, std::memory_order_relaxed);
    rxErrorCountValue.store(0, std::memory_order_relaxed);
    droppedBlockCountValue.store(0, std::memory_order_relaxed);
    running.store(1, std::memory_order_release);

    if (!AudioI2SPort::start(i2sPins.controller, AudioI2SPort::Direction::Rx)) {
        running.store(0, std::memory_order_release);
        AudioI2SPort::clearEventHandler(i2sPins.controller, AudioI2SPort::Direction::Rx, this);
        if (!AudioI2SPort::stop(i2sPins.controller, AudioI2SPort::Direction::Rx)) {
            ESP_LOGE(TAG, "RX channel could not be stopped after startup failure; retaining port handle");
            return false;
        }
        if (!AudioI2SPort::release(i2sPins.controller, AudioI2SPort::Direction::Rx)) {
            ESP_LOGE(TAG, "Could not release RX port after startup failure; retaining handle");
            return false;
        }
        rxHandle = nullptr;

        writeBlockIndex.store(0, std::memory_order_relaxed);
        readBlockIndex.store(0, std::memory_order_relaxed);
        return false;
    }

    return true;
}

OSIZE
void AudioInputI2S::end() {
    audio_detail::GraphUpdateLock graphLock;
    if (!isRunning() && rxHandle == nullptr) return;

    // Stop accepting samples, then detach and drain the ISR callback before
    // resetting the ring. This keeps lifecycle resets out of the hot ISR path.
    running.store(0, std::memory_order_release);

    if (rxHandle != nullptr) {
        AudioI2SPort::clearEventHandler(i2sPins.controller, AudioI2SPort::Direction::Rx, this);
        if (!AudioI2SPort::stop(i2sPins.controller, AudioI2SPort::Direction::Rx)) {
            ESP_LOGE(TAG, "RX channel could not be stopped; retaining port handle for retry");
            return;
        }
        if (!AudioI2SPort::release(i2sPins.controller, AudioI2SPort::Direction::Rx)) {
            ESP_LOGE(TAG, "Could not release RX port; retaining handle for retry");
            return;
        }
        rxHandle = nullptr;
    }

    writeBlockIndex.store(0, std::memory_order_relaxed);
    readBlockIndex.store(0, std::memory_order_relaxed);
}

OSPEED
bool AUDIO_I2S_ISR_ATTR AudioInputI2S::onI2SReceive(i2s_chan_handle_t, i2s_event_data_t *event, void *userContext) {
    auto *input = static_cast<AudioInputI2S *>(userContext);
    if (input == nullptr || event == nullptr || event->dma_buf == nullptr) return false;
    if (input->running.load(std::memory_order_acquire) == 0) return false;

    // dma_frame_num is AUDIO_BLOCK_SAMPLES and the slots are 16-bit stereo:
    // one valid RX callback is exactly one interleaved AudioStream block.
    if (event->size != sizeof(input->rxSampleRing[0])) {
        input->rxErrorCountValue.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const uint32_t write = input->writeBlockIndex.load(std::memory_order_relaxed);
    const uint32_t read = input->readBlockIndex.load(std::memory_order_acquire);

    if (write - read >= RX_RING_BLOCKS) {
        // Never overwrite data the consumer may be copying. Keep queued blocks
        // and drop this newest DMA block instead.
        input->droppedBlockCountValue.fetch_add(1, std::memory_order_relaxed);
    } else {
        const uint32_t ringIndex = write & (RX_RING_BLOCKS - 1U);
        const int16_t *src = static_cast<const int16_t *>(event->dma_buf);
        int16_t *dst = input->rxSampleRing[ringIndex];

        // The destination block is contiguous: no per-frame modulo is needed.
        for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES * 2U; ++i) dst[i] = src[i];

        input->writeBlockIndex.store(write + 1U, std::memory_order_release);
    }

    input->rxBlockCountValue.fetch_add(1, std::memory_order_relaxed);

    // Clock the graph once per DMA block even when this capture block had to
    // be dropped, so other streams do not stop advancing.
    if (!AudioI2SPort::isClockOwner(input->i2sPins.controller, AudioI2SPort::Direction::Rx)) return false;
    return AudioStream::update_all_from_isr();
}

OSPEED
void AudioInputI2S::update() {
    alignas(4) int16_t captured[AUDIO_BLOCK_SAMPLES * 2];

    const uint32_t read = readBlockIndex.load(std::memory_order_relaxed);
    const uint32_t write = writeBlockIndex.load(std::memory_order_acquire);
    if (write == read) return;

    const int16_t *src = rxSampleRing[read & (RX_RING_BLOCKS - 1U)];
GCC_UNROLL
    for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        captured[i * 2U] = src[i * 2U];
        captured[i * 2U + 1U] = src[i * 2U + 1U];
    }
    // Publish the slot as free only after its samples have been copied.
    readBlockIndex.store(read + 1U, std::memory_order_release);

    // Allocate both channel blocks or neither, so left and right cannot drift
    // when the shared audio memory pool is temporarily exhausted.
    audio_block_t *left = allocate();
    audio_block_t *right = left != nullptr ? allocate() : nullptr;

    if (left == nullptr || right == nullptr) {
        if (left != nullptr) release(left);
        if (right != nullptr) release(right);

        droppedBlockCountValue.fetch_add(1, std::memory_order_relaxed);
        return;
    }

GCC_UNROLL
    for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        left->data[i] = captured[i * 2U];
        right->data[i] = captured[i * 2U + 1U];
    }

    transmit(left, 0);
    transmit(right, 1);
    release(left);
    release(right);
}
