#include "input_i2s.h"

#include <esp_log.h>
#include "i2s_port.h"

namespace {
constexpr char TAG[] = "AudioInputI2S";
}

AudioInputI2S::AudioInputI2S() : AudioStream(0, nullptr) {}

AudioInputI2S::AudioInputI2S(const Pins &pins)
    : AudioStream(0, nullptr), i2sPins(pins) {}

AudioInputI2S::~AudioInputI2S() {
    end();
}

bool AudioInputI2S::isRunning() const {
    return running.load(std::memory_order_acquire) != 0;
}

uint32_t AudioInputI2S::rxBlockCount() const {
    return rxBlockCountValue.load(std::memory_order_relaxed);
}

uint32_t AudioInputI2S::rxErrorCount() const {
    return rxErrorCountValue.load(std::memory_order_relaxed);
}

uint32_t AudioInputI2S::droppedBlockCount() const {
    return droppedBlockCountValue.load(std::memory_order_relaxed);
}

bool AudioInputI2S::begin() {
    return beginInternal();
}

bool AudioInputI2S::begin(const Pins &pins) {
    end();
    i2sPins = pins;
    return beginInternal();
}

bool AudioInputI2S::beginHardware() {
    return beginInternal();
}

bool AudioInputI2S::beginInternal() {
    if (isRunning()) return true;

    if (i2sPins.bclk < 0 || i2sPins.ws < 0 || i2sPins.din < 0) {
        ESP_LOGE(TAG, "Invalid I2S pins: BCLK=%d WS=%d DIN=%d",
                 i2sPins.bclk, i2sPins.ws, i2sPins.din);
        return false;
    }

    const AudioI2SPort::Pins portPins{
        i2sPins.bclk, i2sPins.ws, -1, i2sPins.din, i2sPins.mclk, i2sPins.controller
    };

    if (!AudioI2SPort::acquireRx(portPins, &rxHandle)) {
        rxHandle = nullptr;
        return false;
    }

    if (!AudioI2SPort::setEventHandler(i2sPins.controller, AudioI2SPort::Direction::Rx,
                                       &AudioInputI2S::onI2SReceive, this)) {
        ESP_LOGE(TAG, "Could not register RX event handler");
        AudioI2SPort::release(i2sPins.controller, AudioI2SPort::Direction::Rx);
        rxHandle = nullptr;
        return false;
    }

    portENTER_CRITICAL(&rxMux);
    writeFrameIndex.store(0, std::memory_order_relaxed);
    readFrameIndex.store(0, std::memory_order_relaxed);
    clockFrameRemainder = 0;
    portEXIT_CRITICAL(&rxMux);
    rxBlockCountValue.store(0, std::memory_order_relaxed);
    rxErrorCountValue.store(0, std::memory_order_relaxed);
    droppedBlockCountValue.store(0, std::memory_order_relaxed);
    running.store(1, std::memory_order_release);

    if (!AudioI2SPort::start(i2sPins.controller, AudioI2SPort::Direction::Rx)) {
        running.store(0, std::memory_order_release);
        AudioI2SPort::clearEventHandler(i2sPins.controller, AudioI2SPort::Direction::Rx, this);
        AudioI2SPort::stop(i2sPins.controller, AudioI2SPort::Direction::Rx);
        AudioI2SPort::release(i2sPins.controller, AudioI2SPort::Direction::Rx);
        rxHandle = nullptr;

        portENTER_CRITICAL(&rxMux);
        writeFrameIndex.store(0, std::memory_order_relaxed);
        readFrameIndex.store(0, std::memory_order_relaxed);
        clockFrameRemainder = 0;
        portEXIT_CRITICAL(&rxMux);
        return false;
    }

    return true;
}

void AudioInputI2S::end() {
    if (!isRunning() && rxHandle == nullptr) return;

    // Stop accepting samples, then detach and drain the ISR callback before
    // resetting the ring. This keeps lifecycle resets out of the hot ISR path.
    running.store(0, std::memory_order_release);

    if (rxHandle != nullptr) {
        AudioI2SPort::clearEventHandler(i2sPins.controller, AudioI2SPort::Direction::Rx, this);
        AudioI2SPort::stop(i2sPins.controller, AudioI2SPort::Direction::Rx);
        AudioI2SPort::release(i2sPins.controller, AudioI2SPort::Direction::Rx);
        rxHandle = nullptr;
    }

    portENTER_CRITICAL(&rxMux);
    writeFrameIndex.store(0, std::memory_order_relaxed);
    readFrameIndex.store(0, std::memory_order_relaxed);
    clockFrameRemainder = 0;
    portEXIT_CRITICAL(&rxMux);
}

bool IRAM_ATTR AudioInputI2S::onI2SReceive(i2s_chan_handle_t,
                                          i2s_event_data_t *event,
                                          void *userContext) {
    auto *input = static_cast<AudioInputI2S *>(userContext);
    if (input == nullptr || event == nullptr || event->dma_buf == nullptr) {
        return false;
    }

    constexpr size_t BYTES_PER_STEREO_FRAME = 2U * sizeof(int16_t);
    const size_t bytes = event->size;
    if (bytes == 0 || (bytes % BYTES_PER_STEREO_FRAME) != 0) {
        if (input->running.load(std::memory_order_acquire) != 0) {
            input->rxErrorCountValue.fetch_add(1, std::memory_order_relaxed);
        }
        return false;
    }

    const size_t frames = bytes / BYTES_PER_STEREO_FRAME;
    if (frames > RX_RING_FRAMES) {
        if (input->running.load(std::memory_order_acquire) != 0) {
            input->rxErrorCountValue.fetch_add(1, std::memory_order_relaxed);
        }
        return false;
    }

    const auto *src = static_cast<const int16_t *>(event->dma_buf);
    if (input->running.load(std::memory_order_acquire) == 0) return false;

    // Normally the ISR is the only writer and publishes the completed sample
    // range without taking a lock. In the rare overrun case, synchronize with
    // update() before advancing the consumer index and preserving newest data.
    uint32_t write = input->writeFrameIndex.load(std::memory_order_relaxed);
    uint32_t read = input->readFrameIndex.load(std::memory_order_acquire);
    if ((write - read) + frames > RX_RING_FRAMES) {
        portENTER_CRITICAL_ISR(&input->rxMux);
        write = input->writeFrameIndex.load(std::memory_order_relaxed);
        read = input->readFrameIndex.load(std::memory_order_acquire);

        if ((write - read) + frames > RX_RING_FRAMES) {
            input->readFrameIndex.store(write, std::memory_order_release);
            input->droppedBlockCountValue.fetch_add(1, std::memory_order_relaxed);
        }

        for (size_t i = 0; i < frames; ++i) {
            const uint32_t ringFrame = write % RX_RING_FRAMES;
            input->rxSampleRing[ringFrame * 2U] = src[i * 2U];
            input->rxSampleRing[ringFrame * 2U + 1U] = src[i * 2U + 1U];
            ++write;
        }
        input->writeFrameIndex.store(write, std::memory_order_release);
        portEXIT_CRITICAL_ISR(&input->rxMux);
    } else {
        for (size_t i = 0; i < frames; ++i) {
            const uint32_t ringFrame = write % RX_RING_FRAMES;
            input->rxSampleRing[ringFrame * 2U] = src[i * 2U];
            input->rxSampleRing[ringFrame * 2U + 1U] = src[i * 2U + 1U];
            ++write;
        }
        input->writeFrameIndex.store(write, std::memory_order_release);
    }

    input->clockFrameRemainder += static_cast<uint32_t>(frames);
    const uint32_t completedBlocks = input->clockFrameRemainder / AUDIO_BLOCK_SAMPLES;
    input->clockFrameRemainder %= AUDIO_BLOCK_SAMPLES;
    if (completedBlocks != 0) {
        input->rxBlockCountValue.fetch_add(completedBlocks, std::memory_order_relaxed);
    }

    // Only the shared I2S clock owner may clock the graph. The TX callback
    // performs this job when TX owns the clock, avoiding duplicate updates.
    if (completedBlocks != 0 && AudioI2SPort::isClockOwner(input->i2sPins.controller, AudioI2SPort::Direction::Rx)) {
        bool taskWoken = false;
        for (uint32_t i = 0; i < completedBlocks; ++i) {
            taskWoken |= AudioStream::update_all_from_isr();
        }
        return taskWoken;
    }
    return false;
}

void AudioInputI2S::update() {
    alignas(4) int16_t captured[AUDIO_BLOCK_SAMPLES * 2];

    portENTER_CRITICAL(&rxMux);
    uint32_t read = readFrameIndex.load(std::memory_order_relaxed);
    const uint32_t write = writeFrameIndex.load(std::memory_order_acquire);
    if (write - read < AUDIO_BLOCK_SAMPLES) {
        portEXIT_CRITICAL(&rxMux);
        return;
    }

    for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        const uint32_t ringFrame = read % RX_RING_FRAMES;
        captured[i * 2U] = rxSampleRing[ringFrame * 2U];
        captured[i * 2U + 1U] = rxSampleRing[ringFrame * 2U + 1U];
        ++read;
    }
    readFrameIndex.store(read, std::memory_order_release);
    portEXIT_CRITICAL(&rxMux);

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

    for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        left->data[i] = captured[i * 2U];
        right->data[i] = captured[i * 2U + 1U];
    }

    transmit(left, 0);
    transmit(right, 1);
    release(left);
    release(right);
}
