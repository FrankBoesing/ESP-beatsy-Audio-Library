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
    if (running) return true;

    if (i2sPins.bclk < 0 || i2sPins.ws < 0 || i2sPins.din < 0) {
        ESP_LOGE(TAG, "Invalid I2S pins: BCLK=%d WS=%d DIN=%d",
                 i2sPins.bclk, i2sPins.ws, i2sPins.din);
        return false;
    }

    const AudioI2SPort::Pins portPins{
        i2sPins.bclk, i2sPins.ws, -1, i2sPins.din, i2sPins.mclk
    };

    if (!AudioI2SPort::acquireRx(portPins, &rxHandle)) {
        rxHandle = nullptr;
        return false;
    }

    if (!AudioI2SPort::setEventHandler(AudioI2SPort::Direction::Rx,
                                       &AudioInputI2S::onI2SReceive, this)) {
        ESP_LOGE(TAG, "Could not register RX event handler");
        AudioI2SPort::release(AudioI2SPort::Direction::Rx);
        rxHandle = nullptr;
        return false;
    }

    portENTER_CRITICAL(&rxMux);
    writeFrameIndex = 0;
    readFrameIndex = 0;
    clockFrameRemainder = 0;
    portEXIT_CRITICAL(&rxMux);

    rxBlockCountValue = 0;
    rxErrorCountValue = 0;
    droppedBlockCountValue = 0;
    running = true;

    if (!AudioI2SPort::start(AudioI2SPort::Direction::Rx)) {
        running = false;
        AudioI2SPort::clearEventHandler(AudioI2SPort::Direction::Rx, this);
        AudioI2SPort::stop(AudioI2SPort::Direction::Rx);
        AudioI2SPort::release(AudioI2SPort::Direction::Rx);
        rxHandle = nullptr;
        return false;
    }

    return true;
}

void AudioInputI2S::end() {
    if (!running && rxHandle == nullptr) return;

    running = false;

    if (rxHandle != nullptr) {
        AudioI2SPort::clearEventHandler(AudioI2SPort::Direction::Rx, this);
        AudioI2SPort::stop(AudioI2SPort::Direction::Rx);
        AudioI2SPort::release(AudioI2SPort::Direction::Rx);
        rxHandle = nullptr;
    }

    portENTER_CRITICAL(&rxMux);
    writeFrameIndex = 0;
    readFrameIndex = 0;
    clockFrameRemainder = 0;
    portEXIT_CRITICAL(&rxMux);
}

bool IRAM_ATTR AudioInputI2S::onI2SReceive(i2s_chan_handle_t,
                                          i2s_event_data_t *event,
                                          void *userContext) {
    auto *input = static_cast<AudioInputI2S *>(userContext);
    if (input == nullptr || !input->running || event == nullptr || event->dma_buf == nullptr) {
        return false;
    }

    constexpr size_t BYTES_PER_STEREO_FRAME = 2U * sizeof(int16_t);
    const size_t bytes = event->size;
    if (bytes == 0 || (bytes % BYTES_PER_STEREO_FRAME) != 0) {
        ++input->rxErrorCountValue;
        return false;
    }

    const size_t frames = bytes / BYTES_PER_STEREO_FRAME;
    if (frames > RX_RING_FRAMES) {
        ++input->rxErrorCountValue;
        return false;
    }

    const auto *src = static_cast<const int16_t *>(event->dma_buf);
    uint32_t completedBlocks = 0;

    portENTER_CRITICAL_ISR(&input->rxMux);

    const uint32_t usedFrames = input->writeFrameIndex - input->readFrameIndex;
    if (usedFrames + frames > RX_RING_FRAMES) {
        // Keep the newest samples rather than letting the capture latency grow.
        input->readFrameIndex = input->writeFrameIndex;
        ++input->droppedBlockCountValue;
    }

    uint32_t write = input->writeFrameIndex;
    for (size_t i = 0; i < frames; ++i) {
        const uint32_t ringFrame = write % RX_RING_FRAMES;
        input->rxSampleRing[ringFrame * 2U] = src[i * 2U];
        input->rxSampleRing[ringFrame * 2U + 1U] = src[i * 2U + 1U];
        ++write;
    }
    input->writeFrameIndex = write;

    input->clockFrameRemainder += static_cast<uint32_t>(frames);
    completedBlocks = input->clockFrameRemainder / AUDIO_BLOCK_SAMPLES;
    input->clockFrameRemainder %= AUDIO_BLOCK_SAMPLES;
    input->rxBlockCountValue += completedBlocks;

    portEXIT_CRITICAL_ISR(&input->rxMux);

    // Only the shared I2S clock owner may clock the graph. The TX callback
    // performs this job when TX owns the clock, avoiding duplicate updates.
    if (completedBlocks != 0 && AudioI2SPort::isClockOwner(AudioI2SPort::Direction::Rx)) {
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
    if (writeFrameIndex - readFrameIndex < AUDIO_BLOCK_SAMPLES) {
        portEXIT_CRITICAL(&rxMux);
        return;
    }

    uint32_t read = readFrameIndex;
    for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        const uint32_t ringFrame = read % RX_RING_FRAMES;
        captured[i * 2U] = rxSampleRing[ringFrame * 2U];
        captured[i * 2U + 1U] = rxSampleRing[ringFrame * 2U + 1U];
        ++read;
    }
    readFrameIndex = read;
    portEXIT_CRITICAL(&rxMux);

    // Allocate both channel blocks or neither, so left and right cannot drift
    // when the shared audio memory pool is temporarily exhausted.
    audio_block_t *left = allocate();
    audio_block_t *right = left != nullptr ? allocate() : nullptr;

    if (left == nullptr || right == nullptr) {
        if (left != nullptr) release(left);
        if (right != nullptr) release(right);

        portENTER_CRITICAL(&rxMux);
        ++droppedBlockCountValue;
        portEXIT_CRITICAL(&rxMux);
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
