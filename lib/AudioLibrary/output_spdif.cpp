#include "output_spdif.h"

#include <cmath>
#include "i2s_port.h"
#include "spdif_bmc_encoder.h"

namespace {
constexpr uint8_t PREAMBLE_B = 0x17; // B preamble polarity for a preceding HIGH level
constexpr uint8_t PREAMBLE_M = 0x1D; // M preamble polarity for a preceding HIGH level
constexpr uint8_t PREAMBLE_W = 0x1B; // W preamble polarity for a preceding HIGH level

// Shared state intentionally matches the static mute_PCM() API of Teensy.
std::atomic<bool> pcmMuted{false};

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

} // namespace

AudioOutputSPDIF::AudioOutputSPDIF() : AudioOutputSPDIF(0, Pins{-1}) {}
AudioOutputSPDIF::AudioOutputSPDIF(const Pins &pins) : AudioOutputSPDIF(0, pins) {}
AudioOutputSPDIF::AudioOutputSPDIF(int8_t dout) : AudioOutputSPDIF(0, Pins{dout}) {}
AudioOutputSPDIF::AudioOutputSPDIF(uint8_t controller, const Pins &pins)
    : AudioStream(2, inputQueueArray), spdifPins(pins), controller_(controller) {}
AudioOutputSPDIF::~AudioOutputSPDIF() { end(); }

bool AudioOutputSPDIF::isRunning() const {
    return running.load(std::memory_order_acquire) != 0;
}

void AudioOutputSPDIF::setRunning(bool value) {
    running.store(value ? 1U : 0U, std::memory_order_release);
}

void AudioOutputSPDIF::updateChannelStatus(float rate) {
    sampleRateCode.store(spdifSampleRateCode(rate), std::memory_order_release);
}

bool AudioOutputSPDIF::begin() {
    return beginInternal();
}

bool AudioOutputSPDIF::begin(const Pins &pins) {
    end();
    spdifPins = pins;
    return beginInternal();
}

bool AudioOutputSPDIF::begin(int8_t dout) {
    return begin(Pins{dout});
}

bool AudioOutputSPDIF::beginHardware() {
    return beginInternal();
}

void AudioOutputSPDIF::mute_PCM(const bool mute) {
    pcmMuted.store(mute, std::memory_order_relaxed);
}

OSIZE
bool AudioOutputSPDIF::setSampleRate(float rate) {
    if (!std::isfinite(rate) || rate <= 0.0f) return false;

    if (txQueue != nullptr) {
        BlockPair pair;
        while (xQueueReceive(txQueue, &pair, 0) == pdTRUE) releasePair(pair);
    }

    if (!AudioI2SPort::setSampleRate(rate)) return false;
    updateChannelStatus(rate);
    return true;
}

OSIZE
bool AudioOutputSPDIF::beginInternal() {
    if (isRunning()) return true;
    if (spdifPins.dout < 0) {
        return false;
    }

    txQueue = xQueueCreate(QUEUE_LENGTH, sizeof(BlockPair));
    if (txQueue == nullptr) return false;

    // SPDIF uses only DATA externally. BCLK and WS remain internal to I2S0.
    const AudioI2SPort::Pins portPins{-1, -1, spdifPins.dout, -1, -1, controller_};
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

    if (!AudioI2SPort::setEventHandler(controller_, AudioI2SPort::Direction::Tx,
                                       &AudioOutputSPDIF::onI2STransmit, this)) {
        AudioI2SPort::release(controller_, AudioI2SPort::Direction::Tx);
        txHandle = nullptr;
        vQueueDelete(txQueue);
        txQueue = nullptr;
        return false;
    }

    frameInBlock = 0;
    updateChannelStatus(AudioStream::sampleRate());
    setRunning(true);

    const BaseType_t result = xTaskCreatePinnedToCore(
        txTaskEntry, "AudioSPDIF", 4096, this, configMAX_PRIORITIES - 2,
        &txTask, AUDIO_PROCESSING_CORE);

    if (result != pdPASS) {
        setRunning(false);
        txTask = nullptr;
        AudioI2SPort::clearEventHandler(controller_, AudioI2SPort::Direction::Tx, this);
        AudioI2SPort::release(controller_, AudioI2SPort::Direction::Tx);
        txHandle = nullptr;
        vQueueDelete(txQueue);
        txQueue = nullptr;
        return false;
    }

    if (!AudioI2SPort::start(controller_, AudioI2SPort::Direction::Tx)) {
        setRunning(false);
        xTaskNotifyGive(txTask);
        const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(200);
        while (txTask != nullptr && static_cast<int32_t>(deadline - xTaskGetTickCount()) > 0) vTaskDelay(1);

        AudioI2SPort::clearEventHandler(controller_, AudioI2SPort::Direction::Tx, this);
        AudioI2SPort::stop(controller_, AudioI2SPort::Direction::Tx);
        AudioI2SPort::release(controller_, AudioI2SPort::Direction::Tx);
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
void AudioOutputSPDIF::end() {
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
        AudioI2SPort::clearEventHandler(controller_, AudioI2SPort::Direction::Tx, this);
        AudioI2SPort::stop(controller_, AudioI2SPort::Direction::Tx);
        AudioI2SPort::release(controller_, AudioI2SPort::Direction::Tx);
        txHandle = nullptr;
    }

    if (txQueue != nullptr) {
        vQueueDelete(txQueue);
        txQueue = nullptr;
    }
}

OSPEED
void AudioOutputSPDIF::update() {
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
            releasePair(oldPair);
        }
        if (xQueueSend(txQueue, &pair, 0) != pdTRUE) {
            releasePair(pair);
        }
    }
}


void AudioOutputSPDIF::releasePair(BlockPair &pair) {
    if (pair.left != nullptr) {
        release(pair.left);
        pair.left = nullptr;
    }
    if (pair.right != nullptr) {
        release(pair.right);
        pair.right = nullptr;
    }
}


template <bool Muted>
OSPEED void AudioOutputSPDIF::encodeBlock(const int16_t *left, const int16_t *right) {
    GCC_UNROLL
    for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        const int16_t leftSample = left ? left[i] : 0;
        const int16_t rightSample = right ? right[i] : 0;
        const uint8_t statusBit = channelStatusBit(frameInBlock);

        const uint8_t leftPreamble = frameInBlock == 0 ? PREAMBLE_B : PREAMBLE_M;
        spdif_bmc::encodeSubframe<Muted>(leftSample, statusBit != 0, leftPreamble,
                                         &encodedBuffer[i * 8U]);
        spdif_bmc::encodeSubframe<Muted>(rightSample, statusBit != 0, PREAMBLE_W,
                                         &encodedBuffer[i * 8U + 4U]);

        if (++frameInBlock >= 192U) frameInBlock = 0;
    }
}

void AudioOutputSPDIF::txTaskEntry(void *arg) {
    static_cast<AudioOutputSPDIF *>(arg)->txTaskLoop();
}

OSPEED
bool AudioOutputSPDIF::onI2STransmit(i2s_chan_handle_t, i2s_event_data_t *, void *userContext) {
    auto *output = static_cast<AudioOutputSPDIF *>(userContext);
    if (output == nullptr || output->running.load(std::memory_order_acquire) == 0) return false;
    if (!AudioI2SPort::isClockOwner(output->controller_, AudioI2SPort::Direction::Tx)) return false;
    return AudioStream::update_all_from_isr();
}

OSPEED
void AudioOutputSPDIF::txTaskLoop() {
    BlockPair pair{nullptr, nullptr};
    while (isRunning()) {
        if (xQueueReceive(txQueue, &pair, pdMS_TO_TICKS(100)) != pdTRUE) continue;

        const int16_t *left = pair.left ? pair.left->data : nullptr;
        const int16_t *right = pair.right ? pair.right->data : nullptr;

        // Select a compile-time-specialized encoder once per block. The normal
        // path has no mute check or additional parity work inside the sample loop.
        if (pcmMuted.load(std::memory_order_relaxed)) {
            encodeBlock<true>(left, right);
        } else {
            encodeBlock<false>(left, right);
        }

        size_t bytesWritten = 0;
        (void)i2s_channel_write(txHandle, encodedBuffer, sizeof(encodedBuffer), &bytesWritten, 100);

        releasePair(pair);
        pair = {nullptr, nullptr};
    }

    while (xQueueReceive(txQueue, &pair, 0) == pdTRUE) releasePair(pair);
    txTask = nullptr;
    vTaskDelete(nullptr);
}
