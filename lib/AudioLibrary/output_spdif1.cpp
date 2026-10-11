#include "output_spdif1.h"

#include <cmath>
#include <cstring>
#include <esp_err.h>
#include <esp_log.h>
#include "i2s_port.h"

namespace {
constexpr char TAG[] = "AudioOutputSPDIF1";
constexpr uint8_t PREAMBLE_B = 0x17; // Left channel, first frame in a 192-frame block
constexpr uint8_t PREAMBLE_M = 0x1D; // Left channel, other frames
constexpr uint8_t PREAMBLE_W = 0x1B; // Right channel
constexpr uint8_t CHANNEL_STATUS_WORD_LENGTH_16BIT = 0x02;

uint8_t spdifSampleRateCode(float rate) {
    const uint32_t hz = static_cast<uint32_t>(std::lround(rate));
    switch (hz) {
        case 32000:  return 0x3;
        case 44100:  return 0x0;
        case 48000:  return 0x2;
        case 88200:  return 0x8;
        case 96000:  return 0xA;
        case 176400: return 0xC;
        case 192000: return 0xE;
        default:     return 0x1; // Sample frequency not indicated
    }
}

uint8_t parity32(uint32_t value) {
    value ^= value >> 16U;
    value ^= value >> 8U;
    value ^= value >> 4U;
    value &= 0x0FU;
    return static_cast<uint8_t>((0x6996U >> value) & 1U);
}

// Build one 64-half-bit S/PDIF subframe. The four preamble patterns below are
// selected for a preceding HIGH level; even parity makes the final BMC phase HIGH.
uint64_t encodeSubframe(int16_t sample, bool channelStatus, uint8_t preamble) {
    // 16-bit PCM is MSB-aligned in the 20-bit audio field (subframe bits 12..27).
    uint32_t raw = static_cast<uint32_t>(static_cast<uint16_t>(sample)) << 12U;
    if (channelStatus) raw |= 1UL << 30U; // C bit; V and U remain zero.

    // P makes bits 4..31 have even parity; the preamble is excluded from parity.
    raw |= static_cast<uint32_t>(parity32(raw & 0x7FFFFFF0UL)) << 31U;

    uint64_t encoded = 0;
    for (int bit = 7; bit >= 0; --bit) {
        encoded = (encoded << 1U) | ((preamble >> bit) & 1U);
    }

    // Data bits are sent LSB first. Each bit cell starts with a transition;
    // a data 1 additionally transitions in the middle of the cell.
    bool phaseHigh = true;
    for (uint8_t bitIndex = 4; bitIndex < 32; ++bitIndex) {
        const bool bit = ((raw >> bitIndex) & 1U) != 0;
        const uint8_t pair = phaseHigh ? (bit ? 0b01 : 0b00) : (bit ? 0b10 : 0b11);
        encoded = (encoded << 2U) | pair;
        if (!bit) phaseHigh = !phaseHigh;
    }

    return encoded;
}

void storeSubframe(uint64_t encoded, uint16_t *dst) {
    // I2S 16-bit slots serialize each halfword MSB-first.
    dst[0] = static_cast<uint16_t>(encoded >> 48U);
    dst[1] = static_cast<uint16_t>(encoded >> 32U);
    dst[2] = static_cast<uint16_t>(encoded >> 16U);
    dst[3] = static_cast<uint16_t>(encoded);
}
} // namespace

AudioOutputSPDIF1::AudioOutputSPDIF1() : AudioStream(2, inputQueueArray) {}
AudioOutputSPDIF1::AudioOutputSPDIF1(const Pins &pins) : AudioStream(2, inputQueueArray), spdifPins(pins) {}
AudioOutputSPDIF1::AudioOutputSPDIF1(int8_t dout) : AudioStream(2, inputQueueArray), spdifPins{dout} {}
AudioOutputSPDIF1::~AudioOutputSPDIF1() { end(); }

bool AudioOutputSPDIF1::isRunning() const {
    return running.load(std::memory_order_acquire) != 0;
}

uint32_t AudioOutputSPDIF1::txCallbackCount() const {
    return txCallbackCountValue.load(std::memory_order_relaxed);
}

void AudioOutputSPDIF1::setRunning(bool value) {
    running.store(value ? 1U : 0U, std::memory_order_release);
}

void AudioOutputSPDIF1::resetTxCallbackCount() {
    txCallbackCountValue.store(0, std::memory_order_relaxed);
}

void AudioOutputSPDIF1::updateChannelStatus(float rate) {
    sampleRateCode.store(spdifSampleRateCode(rate), std::memory_order_release);
}

uint8_t AudioOutputSPDIF1::channelStatusBit(uint16_t frame) const {
    // IEC 60958 consumer status byte 3 is sample rate; byte 4 declares 16-bit
    // audio (maximum 20-bit word length). Status bits are transmitted LSB-first.
    if (frame >= 24U && frame < 28U) {
        return static_cast<uint8_t>((sampleRateCode.load(std::memory_order_relaxed) >> (frame - 24U)) & 1U);
    }
    if (frame >= 32U && frame < 40U) {
        return static_cast<uint8_t>((CHANNEL_STATUS_WORD_LENGTH_16BIT >> (frame - 32U)) & 1U);
    }
    return 0;
}

bool AudioOutputSPDIF1::begin() {
    return beginInternal();
}

bool AudioOutputSPDIF1::begin(const Pins &pins) {
    end();
    spdifPins = pins;
    return beginInternal();
}

bool AudioOutputSPDIF1::begin(int8_t dout) {
    return begin(Pins{dout});
}

bool AudioOutputSPDIF1::beginHardware() {
    return beginInternal();
}

bool AudioOutputSPDIF1::setSampleRate(float rate) {
    if (!std::isfinite(rate) || rate <= 0.0f) return false;

    if (txQueue != nullptr) {
        BlockPair pair;
        while (xQueueReceive(txQueue, &pair, 0) == pdTRUE) releasePair(pair);
    }

    if (!AudioI2SPort::setSampleRate(rate)) return false;
    updateChannelStatus(rate);
    return true;
}

bool AudioOutputSPDIF1::beginInternal() {
    if (isRunning()) return true;
    if (spdifPins.dout < 0) {
        ESP_LOGE(TAG, "Invalid S/PDIF DATA pin: %d", spdifPins.dout);
        return false;
    }

    txQueue = xQueueCreate(QUEUE_LENGTH, sizeof(BlockPair));
    if (txQueue == nullptr) return false;

    // SPDIF uses only DATA externally. BCLK and WS remain internal to I2S0.
    const AudioI2SPort::Pins portPins{-1, -1, spdifPins.dout, -1, -1, CONTROLLER};
    const AudioI2SPort::Format format{
        AudioI2SPort::PeripheralMode::Standard,
        AudioI2SPort::DataEncoding::SpdifBMC,
        2,
        16,
        4
    };

    if (!AudioI2SPort::acquireTx(portPins, &txHandle, format)) {
        vQueueDelete(txQueue);
        txQueue = nullptr;
        txHandle = nullptr;
        return false;
    }

    if (!AudioI2SPort::setEventHandler(CONTROLLER, AudioI2SPort::Direction::Tx,
                                       &AudioOutputSPDIF1::onI2STransmit, this)) {
        ESP_LOGE(TAG, "Could not register S/PDIF TX callback");
        AudioI2SPort::release(CONTROLLER, AudioI2SPort::Direction::Tx);
        txHandle = nullptr;
        vQueueDelete(txQueue);
        txQueue = nullptr;
        return false;
    }

    resetTxCallbackCount();
    frameInBlock = 0;
    updateChannelStatus(AudioStream::sampleRate());
    setRunning(true);

    const BaseType_t result = xTaskCreatePinnedToCore(
        txTaskEntry, "AudioSPDIF1", 4096, this, configMAX_PRIORITIES - 2,
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

void AudioOutputSPDIF1::end() {
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

void AudioOutputSPDIF1::update() {
    BlockPair pair;
    pair.left = receiveReadOnly(0);
    pair.right = receiveReadOnly(1);

    // Keep a valid BMC stream on the wire even when the graph has no source
    // blocks; nullptr channels are encoded as PCM silence by the TX task.
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

void AudioOutputSPDIF1::releasePair(BlockPair &pair) {
    if (pair.left != nullptr) {
        release(pair.left);
        pair.left = nullptr;
    }
    if (pair.right != nullptr) {
        release(pair.right);
        pair.right = nullptr;
    }
}

void AudioOutputSPDIF1::txTaskEntry(void *arg) {
    static_cast<AudioOutputSPDIF1 *>(arg)->txTaskLoop();
}

bool AudioOutputSPDIF1::onI2STransmit(i2s_chan_handle_t, i2s_event_data_t *, void *userContext) {
    auto *output = static_cast<AudioOutputSPDIF1 *>(userContext);
    if (output == nullptr || output->running.load(std::memory_order_acquire) == 0) return false;
    output->txCallbackCountValue.fetch_add(1, std::memory_order_relaxed);
    if (!AudioI2SPort::isClockOwner(CONTROLLER, AudioI2SPort::Direction::Tx)) return false;
    return AudioStream::update_all_from_isr();
}

void AudioOutputSPDIF1::txTaskLoop() {
    static uint32_t writeErrorCount = 0;
    static uint32_t lastErrorLogMs = 0;

    BlockPair pair{nullptr, nullptr};
    while (isRunning()) {
        if (xQueueReceive(txQueue, &pair, pdMS_TO_TICKS(100)) != pdTRUE) continue;

        const int16_t *left = pair.left ? pair.left->data : nullptr;
        const int16_t *right = pair.right ? pair.right->data : nullptr;

        for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            const int16_t leftSample = left ? left[i] : 0;
            const int16_t rightSample = right ? right[i] : 0;
            const uint8_t statusBit = channelStatusBit(frameInBlock);

            const uint8_t leftPreamble = frameInBlock == 0 ? PREAMBLE_B : PREAMBLE_M;
            storeSubframe(encodeSubframe(leftSample, statusBit != 0, leftPreamble), &encodedBuffer[i * 8U]);
            storeSubframe(encodeSubframe(rightSample, statusBit != 0, PREAMBLE_W), &encodedBuffer[i * 8U + 4U]);

            if (++frameInBlock >= 192U) frameInBlock = 0;
        }

        size_t bytesWritten = 0;
        const esp_err_t err = i2s_channel_write(txHandle, encodedBuffer, sizeof(encodedBuffer), &bytesWritten, 100);
        if (err != ESP_OK || bytesWritten != sizeof(encodedBuffer)) {
            const uint32_t now = millis();
            ++writeErrorCount;
            if (lastErrorLogMs == 0 || now - lastErrorLogMs >= 1000) {
                ESP_LOGE(TAG, "TX error #%lu: %s, bytes=%lu/%lu",
                         static_cast<unsigned long>(writeErrorCount), esp_err_to_name(err),
                         static_cast<unsigned long>(bytesWritten), static_cast<unsigned long>(sizeof(encodedBuffer)));
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
