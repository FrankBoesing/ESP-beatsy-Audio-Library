#include "output_spdif1.h"

#include <cmath>
#include <array>
#include <cstring>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>
#include "i2s_port.h"

namespace {
constexpr char TAG[] = "AudioOutputSPDIF1";
constexpr uint8_t PREAMBLE_B = 0x17; // B preamble polarity for a preceding HIGH level
constexpr uint8_t PREAMBLE_M = 0x1D; // M preamble polarity for a preceding HIGH level
constexpr uint8_t PREAMBLE_W = 0x1B; // W preamble polarity for a preceding HIGH level
constexpr uint8_t CHANNEL_STATUS_WORD_LENGTH_16BIT = 0x02;

void updateAtomicMax(std::atomic<uint32_t> &target, uint32_t value) {
    uint32_t current = target.load(std::memory_order_relaxed);
    while (current < value &&
           !target.compare_exchange_weak(current, value,
                                         std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {
    }
}

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

// The table stores each byte's BMC encoding in wire order (MSB first).
// It is generated at compile time from the BMC rules, avoiding a per-bit loop
// in the real-time encoder while keeping the table definition auditable.
constexpr uint16_t makeBmcByte(uint8_t value) {
    uint16_t encoded = 0;
    bool phaseHigh = true;
    for (uint8_t i = 0; i < 8; ++i) {
        const bool bit = ((value >> i) & 1U) != 0;
        const uint8_t pair = phaseHigh ? (bit ? 0b01 : 0b00) : (bit ? 0b10 : 0b11);
        encoded = static_cast<uint16_t>((encoded << 2U) | pair);
        if (!bit) phaseHigh = !phaseHigh;
    }
    return encoded;
}

constexpr uint8_t makeBmcNibble(uint8_t value) {
    uint8_t encoded = 0;
    bool phaseHigh = true;
    for (uint8_t i = 0; i < 4; ++i) {
        const bool bit = ((value >> i) & 1U) != 0;
        const uint8_t pair = phaseHigh ? (bit ? 0b01 : 0b00) : (bit ? 0b10 : 0b11);
        encoded = static_cast<uint8_t>((encoded << 2U) | pair);
        if (!bit) phaseHigh = !phaseHigh;
    }
    return encoded;
}

constexpr std::array<uint16_t, 256> makeBmcByteTable() {
    std::array<uint16_t, 256> table{};
    for (uint16_t i = 0; i < table.size(); ++i)
        table[i] = makeBmcByte(static_cast<uint8_t>(i));
    return table;
}

constexpr std::array<uint8_t, 16> makeBmcNibbleTable() {
    std::array<uint8_t, 16> table{};
    for (uint8_t i = 0; i < table.size(); ++i) table[i] = makeBmcNibble(i);
    return table;
}

constexpr auto BMC_BYTE = makeBmcByteTable();
constexpr auto BMC_NIBBLE = makeBmcNibbleTable();

bool oddParity8(uint8_t value) {
    value ^= value >> 4U;
    value ^= value >> 2U;
    value ^= value >> 1U;
    return (value & 1U) != 0;
}

// Build one 64-half-bit subframe. These preamble polarities are selected for
// a preceding HIGH level; even parity keeps the final BMC phase HIGH. Bits
// 4..31 are transmitted LSB first.
uint64_t encodeSubframe(int16_t sample, bool channelStatus, uint8_t preamble) {
    // 16-bit PCM is MSB-aligned in the 20-bit audio field (subframe bits 12..27).
    uint32_t raw = static_cast<uint32_t>(static_cast<uint16_t>(sample)) << 12U;
    if (channelStatus) raw |= 1UL << 6U; // C bit (subframe bit 6); V and U remain zero.

    // P makes bits 4..31 have even parity; the preamble is excluded from parity.
    raw |= static_cast<uint32_t>(parity32(raw & 0x7FFFFFF0UL)) << 31U;

    uint64_t encoded = preamble;
    bool phaseHigh = true;
    for (uint8_t shift = 4; shift < 28; shift += 8) {
        const uint8_t byte = static_cast<uint8_t>(raw >> shift);
        uint16_t chunk = BMC_BYTE[byte];
        if (!phaseHigh) chunk = static_cast<uint16_t>(~chunk);
        encoded = (encoded << 16U) | chunk;
        phaseHigh ^= oddParity8(byte);
    }

    const uint8_t nibble = static_cast<uint8_t>((raw >> 28U) & 0x0FU);
    uint8_t tail = BMC_NIBBLE[nibble];
    if (!phaseHigh) tail = static_cast<uint8_t>(~tail);
    encoded = (encoded << 8U) | tail;

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

uint32_t AudioOutputSPDIF1::txEncodedBlockCount() const {
    return txEncodedBlockCountValue.load(std::memory_order_relaxed);
}

uint32_t AudioOutputSPDIF1::txQueueDropCount() const {
    return txQueueDropCountValue.load(std::memory_order_relaxed);
}

uint32_t AudioOutputSPDIF1::txWriteErrorCount() const {
    return txWriteErrorCountValue.load(std::memory_order_relaxed);
}

uint32_t AudioOutputSPDIF1::maxEncodeTimeUs() const {
    return maxEncodeTimeUsValue.load(std::memory_order_relaxed);
}

uint32_t AudioOutputSPDIF1::maxWriteTimeUs() const {
    return maxWriteTimeUsValue.load(std::memory_order_relaxed);
}

void AudioOutputSPDIF1::setRunning(bool value) {
    running.store(value ? 1U : 0U, std::memory_order_release);
}

void AudioOutputSPDIF1::resetTxCallbackCount() {
    txCallbackCountValue.store(0, std::memory_order_relaxed);
    txEncodedBlockCountValue.store(0, std::memory_order_relaxed);
    txQueueDropCountValue.store(0, std::memory_order_relaxed);
    txWriteErrorCountValue.store(0, std::memory_order_relaxed);
    maxEncodeTimeUsValue.store(0, std::memory_order_relaxed);
    maxWriteTimeUsValue.store(0, std::memory_order_relaxed);
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
        if (xQueueReceive(txQueue, &oldPair, 0) == pdTRUE) {
            txQueueDropCountValue.fetch_add(1, std::memory_order_relaxed);
            releasePair(oldPair);
        }
        if (xQueueSend(txQueue, &pair, 0) != pdTRUE) {
            txQueueDropCountValue.fetch_add(1, std::memory_order_relaxed);
            releasePair(pair);
        }
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
    static uint32_t lastErrorLogMs = 0;

    BlockPair pair{nullptr, nullptr};
    while (isRunning()) {
        if (xQueueReceive(txQueue, &pair, pdMS_TO_TICKS(100)) != pdTRUE) continue;

        const int16_t *left = pair.left ? pair.left->data : nullptr;
        const int16_t *right = pair.right ? pair.right->data : nullptr;

        const int64_t encodeStartUs = esp_timer_get_time();
        for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            const int16_t leftSample = left ? left[i] : 0;
            const int16_t rightSample = right ? right[i] : 0;
            const uint8_t statusBit = channelStatusBit(frameInBlock);

            const uint8_t leftPreamble = frameInBlock == 0 ? PREAMBLE_B : PREAMBLE_M;
            storeSubframe(encodeSubframe(leftSample, statusBit != 0, leftPreamble), &encodedBuffer[i * 8U]);
            storeSubframe(encodeSubframe(rightSample, statusBit != 0, PREAMBLE_W), &encodedBuffer[i * 8U + 4U]);

            if (++frameInBlock >= 192U) frameInBlock = 0;
        }

        const uint32_t encodeElapsedUs =
            static_cast<uint32_t>(esp_timer_get_time() - encodeStartUs);
        updateAtomicMax(maxEncodeTimeUsValue, encodeElapsedUs);

        size_t bytesWritten = 0;
        const int64_t writeStartUs = esp_timer_get_time();
        const esp_err_t err = i2s_channel_write(txHandle, encodedBuffer, sizeof(encodedBuffer), &bytesWritten, 100);
        const uint32_t writeElapsedUs =
            static_cast<uint32_t>(esp_timer_get_time() - writeStartUs);
        updateAtomicMax(maxWriteTimeUsValue, writeElapsedUs);

        if (err != ESP_OK || bytesWritten != sizeof(encodedBuffer)) {
            const uint32_t errorCount =
                txWriteErrorCountValue.fetch_add(1, std::memory_order_relaxed) + 1;
            const uint32_t now = millis();
            if (lastErrorLogMs == 0 || now - lastErrorLogMs >= 1000) {
                ESP_LOGE(TAG, "TX error #%lu: %s, bytes=%lu/%lu",
                         static_cast<unsigned long>(errorCount), esp_err_to_name(err),
                         static_cast<unsigned long>(bytesWritten), static_cast<unsigned long>(sizeof(encodedBuffer)));
                lastErrorLogMs = now;
            }
        } else {
            txEncodedBlockCountValue.fetch_add(1, std::memory_order_relaxed);
        }

        releasePair(pair);
        pair = {nullptr, nullptr};
    }

    while (xQueueReceive(txQueue, &pair, 0) == pdTRUE) releasePair(pair);
    txTask = nullptr;
    vTaskDelete(nullptr);
}
