#include "output_i2s.h"

#include <cstring>
#include <esp_err.h>
#include <cmath>
#include "i2s_port.h"

static constexpr const char TAG[] = "I2S";

namespace {
// Prevent AudioTask::update() from racing with TX queue creation, draining,
// or deletion. The lock is recursive because AudioMemory() already holds it
// while calling beginHardware().
class AudioGraphUpdateLock {
  public:
    AudioGraphUpdateLock() { AudioNoInterrupts(); }
    ~AudioGraphUpdateLock() { AudioInterrupts(); }

    AudioGraphUpdateLock(const AudioGraphUpdateLock &) = delete;
    AudioGraphUpdateLock &operator=(const AudioGraphUpdateLock &) = delete;
};
} // namespace

AudioOutputI2S::AudioOutputI2S() : AudioStream(2, inputQueueArray) {} // kein Hardware-Init hier
AudioOutputI2S::AudioOutputI2S(const Pins &pins)
    : AudioStream(2, inputQueueArray), i2sPins(pins) {} // kein Hardware-Init hier
AudioOutputI2S::~AudioOutputI2S() { end(); }
bool AudioOutputI2S::isRunning() const { return running.load(std::memory_order_acquire) != 0; }
uint32_t AudioOutputI2S::txCallbackCount() const { return txCallbackCountValue.load(std::memory_order_relaxed); }
void AudioOutputI2S::setRunning(bool value) { running.store(value ? 1U : 0U, std::memory_order_release); }

bool AudioOutputI2S::markTxCallbackFromISR() {
    if (running.load(std::memory_order_acquire) == 0) return false;
    txCallbackCountValue.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void AudioOutputI2S::resetTxCallbackCount() { txCallbackCountValue.store(0, std::memory_order_relaxed); }

bool AudioOutputI2S::begin() { return beginInternal(); }
bool AudioOutputI2S::begin(const Pins &pins) {
    end();
    i2sPins = pins;
    return beginInternal();
}
bool AudioOutputI2S::beginHardware() { return beginInternal(); }

OSIZE
bool AudioOutputI2S::setChannelCount(uint num) {
    if (num < 1 || num > 2) return false;
    AudioGraphUpdateLock graphLock;
    if (isRunning()) return false;
    numChannels = num;
    return true;
}

OSIZE
bool AudioOutputI2S::setSampleRate(float hz) {
    if (!std::isfinite(hz) || hz <= 0.0f) return false;
    AudioGraphUpdateLock graphLock;
    if (hz == AudioStream::sampleRate()) return true;

    const bool writerLocked = txWriteMutex != nullptr;
    if (writerLocked && xSemaphoreTake(txWriteMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGE(TAG, "Timed out waiting for I2S TX writer before rate change");
        return false;
    }

    // Hold the writer lock before draining so no old-rate pair can be removed
    // from the queue and remain in an in-flight i2s_channel_write().
    if (txQueue != nullptr) {
        BlockPair pair;
        while (xQueueReceive(txQueue, &pair, 0) == pdTRUE) {
            releasePair(pair);
        }
    }

    const bool ok = AudioI2SPort::setSampleRate(hz);
    if (writerLocked) xSemaphoreGive(txWriteMutex);
    return ok;
}

OSIZE
bool AudioOutputI2S::beginInternal() {
    AudioGraphUpdateLock graphLock;
    if (isRunning()) return true;
    if (i2sPins.bclk < 0 || i2sPins.ws < 0 || i2sPins.dout < 0) {
        ESP_LOGE(TAG, "Invalid I2S output pins");
        return false;
    }

    txQueue = xQueueCreate(QUEUE_LENGTH, sizeof(BlockPair));
    if (txQueue == nullptr) return false;

    txWriteMutex = xSemaphoreCreateMutex();
    if (txWriteMutex == nullptr) {
        vQueueDelete(txQueue);
        txQueue = nullptr;
        return false;
    }

    const AudioI2SPort::Pins portPins{i2sPins.bclk, i2sPins.ws, i2sPins.dout, -1, i2sPins.mclk, i2sPins.controller};
    if (!AudioI2SPort::acquireTx(portPins, &txHandle)) {
        vQueueDelete(txQueue);
        txQueue = nullptr;
        vSemaphoreDelete(txWriteMutex);
        txWriteMutex = nullptr;
        txHandle = nullptr;
        return false;
    }

    if (!AudioI2SPort::setEventHandler(i2sPins.controller, AudioI2SPort::Direction::Tx, &AudioOutputI2S::onI2STransmit,
                                       this)) {
        ESP_LOGE(TAG, "Could not register TX event handler");
        AudioI2SPort::release(i2sPins.controller, AudioI2SPort::Direction::Tx);
        txHandle = nullptr;
        vQueueDelete(txQueue);
        txQueue = nullptr;
        vSemaphoreDelete(txWriteMutex);
        txWriteMutex = nullptr;
        return false;
    }

    resetTxCallbackCount();
    setRunning(true);

    const BaseType_t result = xTaskCreatePinnedToCore(txTaskEntry, "AudioI2STx", 2048, this, configMAX_PRIORITIES - 2,
                                                      &txTask, AUDIO_PROCESSING_CORE);

    if (result != pdPASS) {
        setRunning(false);
        txTask = nullptr;
        AudioI2SPort::clearEventHandler(i2sPins.controller, AudioI2SPort::Direction::Tx, this);
        AudioI2SPort::release(i2sPins.controller, AudioI2SPort::Direction::Tx);
        txHandle = nullptr;
        vQueueDelete(txQueue);
        txQueue = nullptr;
        vSemaphoreDelete(txWriteMutex);
        txWriteMutex = nullptr;
        return false;
    }

    if (!AudioI2SPort::start(i2sPins.controller, AudioI2SPort::Direction::Tx)) {
        setRunning(false);
        xTaskNotifyGive(txTask);
        // txTask uses finite queue/write timeouts and signals completion by
        // clearing txTask. Never free its queue/handle while it still runs.
        while (txTask != nullptr) {
            vTaskDelay(1);
        }

        AudioI2SPort::clearEventHandler(i2sPins.controller, AudioI2SPort::Direction::Tx, this);
        AudioI2SPort::stop(i2sPins.controller, AudioI2SPort::Direction::Tx);
        AudioI2SPort::release(i2sPins.controller, AudioI2SPort::Direction::Tx);
        txHandle = nullptr;
        if (txQueue != nullptr) {
            vQueueDelete(txQueue);
            txQueue = nullptr;
        }
        if (txWriteMutex != nullptr) {
            vSemaphoreDelete(txWriteMutex);
            txWriteMutex = nullptr;
        }
        return false;
    }

    return true;
}

OSIZE
void AudioOutputI2S::end() {
    AudioGraphUpdateLock graphLock;
    if (!isRunning() && txQueue == nullptr && txHandle == nullptr && txTask == nullptr && txWriteMutex == nullptr) return;

    setRunning(false);

    if (txTask != nullptr) {
        xTaskNotifyGive(txTask);
        // txTask uses finite queue/write timeouts and signals completion by
        // clearing txTask. Do not delete resources if it has not exited yet.
        while (txTask != nullptr) {
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
        AudioI2SPort::stop(i2sPins.controller, AudioI2SPort::Direction::Tx);
        AudioI2SPort::release(i2sPins.controller, AudioI2SPort::Direction::Tx);
        txHandle = nullptr;
    }

    if (txQueue != nullptr) {
        vQueueDelete(txQueue);
        txQueue = nullptr;
    }
    if (txWriteMutex != nullptr) {
        vSemaphoreDelete(txWriteMutex);
        txWriteMutex = nullptr;
    }
}

OSPEED
void AudioOutputI2S::update() {
    BlockPair pair;

    pair.left = receiveReadOnly(0);
    pair.right = receiveReadOnly(1);

    if (pair.left == nullptr && pair.right == nullptr) return;

    if (txQueue == nullptr) {
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

bool AudioOutputI2S::onI2STransmit(i2s_chan_handle_t, i2s_event_data_t *, void *userContext) {
    auto *output = static_cast<AudioOutputI2S *>(userContext);
    if (output == nullptr || !output->markTxCallbackFromISR()) return false;
    if (!AudioI2SPort::isClockOwner(output->i2sPins.controller, AudioI2SPort::Direction::Tx)) return false;
    return AudioStream::update_all_from_isr();
}

OSPEED
void AudioOutputI2S::txTaskLoop() {
    static uint32_t writeErrorCount = 0;
    static uint32_t lastErrorLogMs = 0;

    // Ein uint32_t enthält ein I2S-Frame:
    // linker Slot: 16 Bit, rechter Slot: 16 Bit.
    uint32_t buffer[AUDIO_BLOCK_SAMPLES];
    BlockPair pair;

    while (isRunning()) {
        SemaphoreHandle_t writerMutex = txWriteMutex;
        if (writerMutex != nullptr &&
            xSemaphoreTake(writerMutex, pdMS_TO_TICKS(10)) != pdTRUE) {
            continue;
        }

        if (!isRunning() || xQueueReceive(txQueue, &pair, pdMS_TO_TICKS(10)) != pdTRUE) {
            if (writerMutex != nullptr) xSemaphoreGive(writerMutex);
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

        releasePair(pair);
        if (writerMutex != nullptr) xSemaphoreGive(writerMutex);
    }

    // Beim Beenden alle noch ausstehenden Blöcke freigeben.
    while (xQueueReceive(txQueue, &pair, 0) == pdTRUE) {
        releasePair(pair);
    }

    txTask = nullptr;

    vTaskDelete(nullptr);
}
