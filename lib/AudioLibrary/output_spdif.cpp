#include "output_spdif.h"
#include <esp_memory_utils.h>

#include <cmath>
#include "i2s_port.h"

static constexpr char TAG[] = "SPDIF";

static constexpr uint8_t PREAMBLE_B = 0x17; // B preamble polarity for a preceding HIGH level
static constexpr uint8_t PREAMBLE_M = 0x1D; // M preamble polarity for a preceding HIGH level
static constexpr uint8_t PREAMBLE_W = 0x1B; // W preamble polarity for a preceding HIGH level

static uint8_t spdifSampleRateCode(float rate) {
    const uint32_t hz = (uint32_t)(std::lround(rate));
    switch (hz) {
    case 32000:
        return 0x3;
    case 44100:
        return 0x0;
    case 48000:
        return 0x2;
    case 88200:
        return 0x8;
    case 96000:
        return 0xA;
    case 176400:
        return 0xC;
    case 192000:
        return 0xE;
    default:
        return 0x1; // Sample frequency not indicated
    }
}

AudioOutputSPDIF::AudioOutputSPDIF() : AudioOutputSPDIF(0, Pins{-1}) {}
AudioOutputSPDIF::AudioOutputSPDIF(const Pins &pins) : AudioOutputSPDIF(0, pins) {}
AudioOutputSPDIF::AudioOutputSPDIF(int8_t dout) : AudioOutputSPDIF(0, Pins{dout}) {}
AudioOutputSPDIF::AudioOutputSPDIF(uint8_t controller, const Pins &pins)
    : AudioStream(2, inputQueueArray), spdifPins(pins), controller_(controller) {}
AudioOutputSPDIF::~AudioOutputSPDIF() { end(); }
bool AudioOutputSPDIF::isRunning() const { return running.load(std::memory_order_acquire) != 0; }
void AudioOutputSPDIF::setRunning(bool value) { running.store(value ? 1U : 0U, std::memory_order_release); }

void AudioOutputSPDIF::updateChannelStatus(float rate) {
    sampleRateCode.store(spdifSampleRateCode(rate), std::memory_order_release);
}

bool AudioOutputSPDIF::begin() { return beginInternal(); }
bool AudioOutputSPDIF::begin(const Pins &pins) {
    end();
    if (txHandle != nullptr || txQueue != nullptr) return false;
    spdifPins = pins;
    return beginInternal();
}

bool AudioOutputSPDIF::begin(int8_t dout) { return begin(Pins{dout}); }
bool AudioOutputSPDIF::beginHardware() { return beginInternal(); }

OSIZE
bool AudioOutputSPDIF::setSampleRate(float rate) {
    if (!std::isfinite(rate) || rate <= 0.0f) return false;
    return AudioI2SPort::setSampleRate(rate);
}

OSIZE
bool AudioOutputSPDIF::beginInternal() {
    audio_detail::GraphUpdateLock graphLock;
    if (isRunning()) return true;
#if defined(CONFIG_I2S_ISR_IRAM_SAFE) && CONFIG_I2S_ISR_IRAM_SAFE
    if (!esp_ptr_internal(this)) {
        ESP_LOGE(TAG, "IRAM-safe I2S callbacks require AudioOutputSPDIF in internal RAM");
        return false;
    }
#endif
    if (txHandle != nullptr || txQueue != nullptr) {
        ESP_LOGE(TAG, "Previous SPDIF output shutdown did not complete");
        return false;
    }
    if (spdifPins.dout < 0) {
        return false;
    }

    txQueue = xQueueCreate(QUEUE_LENGTH, sizeof(BlockPair));
    if (txQueue == nullptr) return false;

    // SPDIF uses only DATA externally. BCLK and WS remain internal to I2S0.
    const AudioI2SPort::Pins portPins{-1, -1, spdifPins.dout, -1, -1, controller_};
    const AudioI2SPort::Format format{AudioI2SPort::PeripheralMode::Standard, AudioI2SPort::DataEncoding::SpdifBMC, 2,
                                      16, 4};

    if (!AudioI2SPort::acquireTx(portPins, &txHandle, format)) {
        vQueueDelete(txQueue);
        txQueue = nullptr;
        txHandle = nullptr;
        return false;
    }

    if (!AudioI2SPort::setEventHandler(controller_, AudioI2SPort::Direction::Tx, &AudioOutputSPDIF::onI2STransmit,
                                       this)) {
        if (!AudioI2SPort::release(controller_, AudioI2SPort::Direction::Tx)) {
            ESP_LOGE(TAG, "Could not release SPDIF port after callback registration failure; retaining handle");
            return false;
        }
        txHandle = nullptr;
        vQueueDelete(txQueue);
        txQueue = nullptr;
        return false;
    }

    frameInBlock = 0;
    updateChannelStatus(AudioStream::sampleRate());
    setRunning(true);

    TaskHandle_t createdTask = nullptr;
    const BaseType_t result = xTaskCreatePinnedToCore(txTaskEntry, "AudioSPDIF", 2048, this, configMAX_PRIORITIES - 2,
                                                      &createdTask, AUDIO_PROCESSING_CORE);
    if (result == pdPASS) txTask.store(createdTask, std::memory_order_release);

    if (result != pdPASS) {
        setRunning(false);
        txTask.store(nullptr, std::memory_order_release);
        AudioI2SPort::clearEventHandler(controller_, AudioI2SPort::Direction::Tx, this);
        if (!AudioI2SPort::release(controller_, AudioI2SPort::Direction::Tx)) {
            ESP_LOGE(TAG, "Could not release SPDIF port after task creation failure; retaining resources");
            return false;
        }
        txHandle = nullptr;
        vQueueDelete(txQueue);
        txQueue = nullptr;
        return false;
    }

    if (!AudioI2SPort::start(controller_, AudioI2SPort::Direction::Tx)) {
        setRunning(false);
        // Wait for the finite queue/write timeout to expire before freeing
        // resources referenced by the task; avoid notifying a task that may
        // already have reached its self-delete path.
        while (txTask.load(std::memory_order_acquire) != nullptr) vTaskDelay(1);

        AudioI2SPort::clearEventHandler(controller_, AudioI2SPort::Direction::Tx, this);
        if (!AudioI2SPort::stop(controller_, AudioI2SPort::Direction::Tx)) {
            ESP_LOGE(TAG, "TX channel could not be stopped after startup failure; retaining port resources");
            return false;
        }
        if (!AudioI2SPort::release(controller_, AudioI2SPort::Direction::Tx)) {
            ESP_LOGE(TAG, "Could not release SPDIF port after startup failure; retaining resources");
            return false;
        }
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
    audio_detail::GraphUpdateLock graphLock;
    if (!isRunning() && txQueue == nullptr && txHandle == nullptr &&
        txTask.load(std::memory_order_acquire) == nullptr) return;

    setRunning(false);

    if (txTask.load(std::memory_order_acquire) != nullptr) {
        // Do not delete the queue while txTask may still be using it. The
        // task's finite waits let it observe running=false without a notify.
        while (txTask.load(std::memory_order_acquire) != nullptr) vTaskDelay(1);
    }

    if (txQueue != nullptr) {
        BlockPair pair;
        while (xQueueReceive(txQueue, &pair, 0) == pdTRUE) releasePair(pair);
    }

    if (txHandle != nullptr) {
        AudioI2SPort::clearEventHandler(controller_, AudioI2SPort::Direction::Tx, this);
        if (!AudioI2SPort::stop(controller_, AudioI2SPort::Direction::Tx)) {
            ESP_LOGE(TAG, "TX channel could not be stopped; retaining port resources for retry");
            return;
        }
        if (!AudioI2SPort::release(controller_, AudioI2SPort::Direction::Tx)) {
            ESP_LOGE(TAG, "Could not release SPDIF port; retaining resources for retry");
            return;
        }
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
    // While running, keep a valid BMC stream even when the graph has no source
    // blocks; nullptr channels are encoded as PCM silence by the TX task.
    if (!isRunning() || txQueue == nullptr) {
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


void AudioOutputSPDIF::txTaskEntry(void *arg) { static_cast<AudioOutputSPDIF *>(arg)->txTaskLoop(); }

OSPEED
bool AUDIO_I2S_ISR_ATTR AudioOutputSPDIF::onI2STransmit(i2s_chan_handle_t, i2s_event_data_t *, void *userContext) {
    auto *output = static_cast<AudioOutputSPDIF *>(userContext);
    if (output == nullptr || output->running.load(std::memory_order_acquire) == 0) return false;
    if (!AudioI2SPort::isClockOwner(output->controller_, AudioI2SPort::Direction::Tx)) return false;
    return AudioStream::update_all_from_isr();
}

extern "C" {
    extern const uint32_t BMC_BYTE_TABLE[256];
    extern const uint8_t BMC_NIBBLE_TABLE[16];
}

ALWAYS_INLINE OSPEED
static void encodeSubframe(int16_t sample, bool channelStatus, uint8_t preamble, uint16_t *destination) {
    if (destination == nullptr) return;

    const uint16_t sampleBits = (uint16_t)(sample);
    const uint32_t lowEntry = BMC_BYTE_TABLE[sampleBits & 0xFFU];
    const uint32_t highEntry = BMC_BYTE_TABLE[sampleBits >> 8U];

    const bool lowParity = ((lowEntry >> 16U) & 1U) != 0;
    const bool highParity = ((highEntry >> 16U) & 1U) != 0;

    // IEC 60958 bits 4..31 require even parity. The zero auxiliary field adds
    // no parity, so P is the XOR of the two sample-byte parities and C.
    const bool parityBit = lowParity ^ highParity ^ channelStatus;
    const uint8_t statusNibble = static_cast<uint8_t>((channelStatus ? 0x04U : 0U) | (parityBit ? 0x08U : 0U));

    const uint16_t lowCode = (uint16_t)(lowEntry);
    uint16_t highCode = (uint16_t)(highEntry);

    // Table entries assume a HIGH phase at byte start. An odd-parity byte
    // flips the ending BMC phase; invert the next table word when required.
    bool phaseHigh = true; // the preceding eight auxiliary bits are zero
    phaseHigh ^= lowParity;
    if (!phaseHigh) highCode = (uint16_t)(~highCode);
    phaseHigh ^= highParity;

    uint8_t tail = BMC_NIBBLE_TABLE[statusNibble];
    if (!phaseHigh) tail = static_cast<uint8_t>(~tail);

    // Pack directly into the DMA buffer: no per-bit loop and no 64-bit
    // temporary followed by a separate split/store pass.
    constexpr uint16_t zeroAuxBmc = 0x3333;
    destination[0] = (uint16_t)(((uint16_t)(preamble) << 8U) | (zeroAuxBmc >> 8U));
    destination[1] = (uint16_t)(((zeroAuxBmc & 0xFFU) << 8U) | (lowCode >> 8U));
    destination[2] = (uint16_t)((lowCode << 8U) | (highCode >> 8U));
    destination[3] = (uint16_t)((highCode << 8U) | tail);
}

OSPEED
void AudioOutputSPDIF::txTaskLoop() {
    uint32_t writeErrorCount = 0;
    uint32_t lastErrorLogMs = 0;
    BlockPair pair{nullptr, nullptr};
    while (isRunning()) {
        if (!isRunning() || xQueueReceive(txQueue, &pair, pdMS_TO_TICKS(10)) != pdTRUE) {
            if (!isRunning()) break;
            continue;
        }

        if (!isRunning()) {
            releasePair(pair);
            pair = {nullptr, nullptr};
            break;
        }

        // The sample rate is global and may have been changed through another output.
        updateChannelStatus(AudioStream::sampleRate());

        const uint16_t frameInBlockStart = frameInBlock;
        const int16_t *left = pair.left ? pair.left->data : audio_zeroblock;
        const int16_t *right = pair.right ? pair.right->data : audio_zeroblock;

        auto encodeBlock = [this](const int16_t *leftSamples, const int16_t *rightSamples) {
            for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
                const int16_t leftSample = leftSamples[i];
                const int16_t rightSample = rightSamples[i];
                const uint8_t statusBit = channelStatusBit(frameInBlock);
                const uint8_t leftPreamble = frameInBlock == 0 ? PREAMBLE_B : PREAMBLE_M;

                encodeSubframe(leftSample, statusBit != 0, leftPreamble, &encodedBuffer[i * 8U]);
                encodeSubframe(rightSample, statusBit != 0, PREAMBLE_W, &encodedBuffer[i * 8U + 4U]);

                if (++frameInBlock >= 192U) frameInBlock = 0;
            }
        };
        encodeBlock(left, right);

        size_t bytesWritten = 0;
        if (!isRunning()) {
            frameInBlock = frameInBlockStart;
            releasePair(pair);
            break;
        }

        const esp_err_t err = i2s_channel_write(txHandle, encodedBuffer, sizeof(encodedBuffer), &bytesWritten, 100);
        if (err != ESP_OK || bytesWritten != sizeof(encodedBuffer)) {
            // Keep the status-frame index aligned to complete stereo subframes
            // accepted by the driver, rather than counting an unwritten tail.
            constexpr size_t BYTES_PER_SPDIF_FRAME = 8U * sizeof(uint16_t);
            frameInBlock = (uint16_t)(
                (frameInBlockStart + (bytesWritten / BYTES_PER_SPDIF_FRAME)) % 192U);

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
    txTask.store(nullptr, std::memory_order_release);
    vTaskDelete(nullptr);
}
