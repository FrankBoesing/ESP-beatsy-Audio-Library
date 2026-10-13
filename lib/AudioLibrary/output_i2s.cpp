#include "output_i2s.h"
#include <esp_memory_utils.h>

#include <cstring>
#include <esp_err.h>
#include <cmath>
#include "i2s_port.h"
#include "audio_graph_update_lock.h"

static constexpr const char TAG[] = "I2S";



AudioOutputI2S::AudioOutputI2S() : AudioStream(2, inputQueueArray) {} // kein Hardware-Init hier
AudioOutputI2S::AudioOutputI2S(const Pins &pins)
    : AudioStream(2, inputQueueArray), i2sPins(pins) {} // kein Hardware-Init hier
AudioOutputI2S::~AudioOutputI2S() { end(); }
bool AudioOutputI2S::isRunning() const { return running.load(std::memory_order_acquire) != 0; }
uint32_t AudioOutputI2S::txCallbackCount() const { return txCallbackCountValue.load(std::memory_order_relaxed); }
void AudioOutputI2S::setRunning(bool value) { running.store(value ? 1U : 0U, std::memory_order_release); }

bool AUDIO_I2S_ISR_ATTR AudioOutputI2S::markTxCallbackFromISR() {
    if (running.load(std::memory_order_acquire) == 0) return false;
    txCallbackCountValue.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void AudioOutputI2S::resetTxCallbackCount() { txCallbackCountValue.store(0, std::memory_order_relaxed); }

bool AudioOutputI2S::begin() { return beginInternal(); }
bool AudioOutputI2S::begin(const Pins &pins) {
    end();
    if (txHandle != nullptr || txQueue != nullptr) return false;
    i2sPins = pins;
    return beginInternal();
}
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
    audio_detail::GraphUpdateLock graphLock;
    if (hz == AudioStream::sampleRate()) return true;

    // GraphUpdateLock prevents new blocks from being queued during the change.
    // The shared port writer lock excludes in-flight DMA writes, while the
    // generation stamp makes any old pair already dequeued/queued harmless.
    const uint32_t oldGeneration = AudioI2SPort::currentSampleRateGeneration();
    const bool ok = AudioI2SPort::setSampleRate(hz);
    if (txQueue != nullptr &&
        AudioI2SPort::currentSampleRateGeneration() != oldGeneration) {
        BlockPair pair;
        while (xQueueReceive(txQueue, &pair, 0) == pdTRUE) {
            releasePair(pair);
        }
    }
    return ok;
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
    if (txHandle != nullptr || txQueue != nullptr) {
        ESP_LOGE(TAG, "Previous I2S output shutdown did not complete");
        return false;
    }
    if (i2sPins.bclk < 0 || i2sPins.ws < 0 || i2sPins.dout < 0) {
        ESP_LOGE(TAG, "Invalid I2S output pins");
        return false;
    }

    txQueue = xQueueCreate(QUEUE_LENGTH, sizeof(BlockPair));
    if (txQueue == nullptr) return false;

    const AudioI2SPort::Pins portPins{i2sPins.bclk, i2sPins.ws, i2sPins.dout, -1, i2sPins.mclk, i2sPins.controller};
    if (!AudioI2SPort::acquireTx(portPins, &txHandle)) {
        vQueueDelete(txQueue);
        txQueue = nullptr;
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
        vQueueDelete(txQueue);
        txQueue = nullptr;
        return false;
    }

    resetTxCallbackCount();
    setRunning(true);

    TaskHandle_t createdTask = nullptr;
    const BaseType_t result = xTaskCreatePinnedToCore(txTaskEntry, "AudioI2STx", 2048, this, configMAX_PRIORITIES - 2,
                                                      &createdTask, AUDIO_PROCESSING_CORE);
    if (result == pdPASS) txTask.store(createdTask, std::memory_order_release);

    if (result != pdPASS) {
        setRunning(false);
        txTask.store(nullptr, std::memory_order_release);
        AudioI2SPort::clearEventHandler(i2sPins.controller, AudioI2SPort::Direction::Tx, this);
        if (!AudioI2SPort::release(i2sPins.controller, AudioI2SPort::Direction::Tx)) {
            ESP_LOGE(TAG, "Could not release TX port after task creation failure; retaining resources");
            return false;
        }
        txHandle = nullptr;
        vQueueDelete(txQueue);
        txQueue = nullptr;
        return false;
    }

    if (!AudioI2SPort::start(i2sPins.controller, AudioI2SPort::Direction::Tx)) {
        setRunning(false);
        // The task's finite queue/write timeouts guarantee that it can exit
        // without racing a notification against its own deletion.
        while (txTask.load(std::memory_order_acquire) != nullptr) {
            vTaskDelay(1);
        }

        AudioI2SPort::clearEventHandler(i2sPins.controller, AudioI2SPort::Direction::Tx, this);
        if (!AudioI2SPort::stop(i2sPins.controller, AudioI2SPort::Direction::Tx)) {
            ESP_LOGE(TAG, "TX channel could not be stopped after startup failure; retaining port resources");
            return false;
        }
        if (!AudioI2SPort::release(i2sPins.controller, AudioI2SPort::Direction::Tx)) {
            ESP_LOGE(TAG, "Could not release TX port after startup failure; retaining resources");
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
void AudioOutputI2S::end() {
    audio_detail::GraphUpdateLock graphLock;
    if (!isRunning() && txQueue == nullptr && txHandle == nullptr &&
        txTask.load(std::memory_order_acquire) == nullptr) return;

    setRunning(false);

    if (txTask.load(std::memory_order_acquire) != nullptr) {
        // Do not race a notification against task self-deletion.
        while (txTask.load(std::memory_order_acquire) != nullptr) {
            vTaskDelay(1);
        }
    }

    if (txQueue != nullptr) {
        BlockPair pair;
        while (xQueueReceive(txQueue, &pair, 0) == pdTRUE) {
            releasePair(pair);
        }
    }

    if (txHandle != nullptr) {
        AudioI2SPort::clearEventHandler(i2sPins.controller, AudioI2SPort::Direction::Tx, this);
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

    if (txQueue != nullptr) {
        vQueueDelete(txQueue);
        txQueue = nullptr;
    }
}

OSPEED
void AudioOutputI2S::update() {
    BlockPair pair;

    pair.left = receiveReadOnly(0);
    pair.right = receiveReadOnly(1);

    if (pair.left == nullptr && pair.right == nullptr) return;
    pair.rateGeneration = AudioI2SPort::currentSampleRateGeneration();

    if (!isRunning() || txQueue == nullptr) {
        releasePair(pair);
        return;
    }

    /*
     * The AudioStream scheduler must never wait for I2S hardware.
     * If the queue is full, discard the oldest pair and enqueue the
     * newest one.
     */
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

void AudioOutputI2S::txTaskEntry(void *arg) { static_cast<AudioOutputI2S *>(arg)->txTaskLoop(); }

bool AUDIO_I2S_ISR_ATTR AudioOutputI2S::onI2STransmit(i2s_chan_handle_t, i2s_event_data_t *, void *userContext) {
    auto *output = static_cast<AudioOutputI2S *>(userContext);
    if (output == nullptr || !output->markTxCallbackFromISR()) return false;
    if (!AudioI2SPort::isClockOwner(output->i2sPins.controller, AudioI2SPort::Direction::Tx)) return false;
    return AudioStream::update_all_from_isr();
}

OSPEED
void AudioOutputI2S::txTaskLoop() {
    uint32_t writeErrorCount = 0;
    uint32_t lastErrorLogMs = 0;

    // Ein uint32_t enthält ein I2S-Frame:
    // linker Slot: 16 Bit, rechter Slot: 16 Bit.
    uint32_t buffer[AUDIO_BLOCK_SAMPLES];
    BlockPair pair;

    while (isRunning()) {
        if (!isRunning() || xQueueReceive(txQueue, &pair, pdMS_TO_TICKS(10)) != pdTRUE) {
            if (!isRunning()) break;
            continue;
        }

        // Discard blocks that were queued before a global rate change.
        if (!isRunning() ||
            pair.rateGeneration != AudioI2SPort::currentSampleRateGeneration()) {
            releasePair(pair);
            pair = {nullptr, nullptr, 0};
            if (!isRunning()) break;
            continue;
        }

        if (numChannels == 1) {
            const int16_t *__restrict left = pair.left ? pair.left->data : nullptr;
            const int16_t *__restrict right = pair.right ? pair.right->data : nullptr;
            // Mono: L/R mitteln und dasselbe Sample auf beide Slots legen.
            //TODO: Check this in godbolt. Is it optimized?
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

                // Zweierkomplement-Sample als 16-Bit-Wert übernehmen.
                const uint32_t sample = static_cast<uint16_t>(static_cast<int16_t>(mono));

                buffer[i] = sample | (sample << 16);
            }
        } else {
            // Stereo: linker und rechter Kanal bleiben getrennt.
            const int16_t *left = pair.left ? pair.left->data : audio_zeroblock;
            const int16_t *right = pair.right ? pair.right->data : audio_zeroblock;

            GCC_UNROLL
            for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
                const uint32_t l = static_cast<uint16_t>(left[i]);
                const uint32_t r = static_cast<uint16_t>(right[i]);

                buffer[i] = l | (r << 16);
            }
        }

        size_t bytesWritten = 0;

        /*
         * Der blockierende I2S-Aufruf bleibt ausschließlich im TX-Task.
         * Ein Timeout verhindert, dass das Beenden dauerhaft blockiert,
         * falls der I2S-Treiber keine Daten mehr annimmt.
         */
        if (!AudioI2SPort::lockForWrite(pdMS_TO_TICKS(500))) {
            releasePair(pair);
            pair = {nullptr, nullptr, 0};
            continue;
        }
        if (!isRunning() ||
            pair.rateGeneration != AudioI2SPort::currentSampleRateGeneration()) {
            releasePair(pair);
            pair = {nullptr, nullptr, 0};
            AudioI2SPort::unlockAfterWrite();
            if (!isRunning()) break;
            continue;
        }

        const esp_err_t err = i2s_channel_write(txHandle, buffer, sizeof(buffer), &bytesWritten, 100);

        if (err != ESP_OK || bytesWritten != sizeof(buffer)) {
            const uint32_t now = millis();
            ++writeErrorCount;

            if (lastErrorLogMs == 0 || now - lastErrorLogMs >= 1000) {
                ESP_LOGE(TAG, "TX error #%lu: %s, bytes=%lu/%lu", (unsigned long)writeErrorCount, esp_err_to_name(err),
                         (unsigned long)bytesWritten, (unsigned long)sizeof(buffer));

                lastErrorLogMs = now;
            }
        }

        AudioI2SPort::unlockAfterWrite();
        releasePair(pair);
    }

    // Beim Beenden alle noch ausstehenden Blöcke freigeben.
    while (xQueueReceive(txQueue, &pair, 0) == pdTRUE) {
        releasePair(pair);
    }

    txTask.store(nullptr, std::memory_order_release);

    vTaskDelete(nullptr);
}
