#include "output_i2s.h"

#include <cstring>
#include <esp_err.h>
#include <cmath>
#include "i2s_port.h"

static constexpr const char TAG[] = "I2S";

AudioOutputI2S::AudioOutputI2S() : AudioStream(2, inputQueueArray) {} // kein Hardware-Init hier
AudioOutputI2S::AudioOutputI2S(const Pins &pins)
    : AudioStream(2, inputQueueArray), i2sPins(pins) {} // kein Hardware-Init hier
AudioOutputI2S::~AudioOutputI2S() { end(); }

//bool AudioOutputI2S::begin() { return beginInternal(); }
bool AudioOutputI2S::begin(const Pins &pins) {
    end();
    i2sPins = pins;
    return beginInternal();
}
bool AudioOutputI2S::beginHardware() { return beginInternal(); }

OSIZE
bool AudioOutputI2S::setChannelCount(uint num) {
    if (num < 1 || num > 2) return false;
    numChannels = num;
    return true;
}

OSIZE
bool AudioOutputI2S::setSampleRate(float hz) {
    if (!std::isfinite(hz) || hz <= 0.0f) return false;
    if (hz == AudioStream::sampleRate()) return true;

    // Do not let queued blocks from the previous rate leak into the new clock.
    if (txQueue != nullptr) {
        BlockPair pair;
        while (xQueueReceive(txQueue, &pair, 0) == pdTRUE) {
            releasePair(pair);
        }
    }

    return AudioI2SPort::setSampleRate(hz);
}

OSIZE
bool AudioOutputI2S::beginInternal() {
    if (running) return true;
    if (i2sPins.bclk < 0 || i2sPins.ws < 0 || i2sPins.dout < 0) {
        ESP_LOGE(TAG, "Invalid I2S output pins");
        return false;
    }

    txQueue = xQueueCreate(QUEUE_LENGTH, sizeof(BlockPair));
    if (txQueue == nullptr) return false;

    const AudioI2SPort::Pins portPins{
        i2sPins.bclk, i2sPins.ws, i2sPins.dout, -1, i2sPins.mclk
    };
    if (!AudioI2SPort::acquireTx(portPins, &txHandle)) {
        vQueueDelete(txQueue);
        txQueue = nullptr;
        txHandle = nullptr;
        return false;
    }

    if (!AudioI2SPort::setEventHandler(AudioI2SPort::Direction::Tx,
                                       &AudioOutputI2S::onI2STransmit, this)) {
        ESP_LOGE(TAG, "Could not register TX event handler");
        AudioI2SPort::release(AudioI2SPort::Direction::Tx);
        txHandle = nullptr;
        vQueueDelete(txQueue);
        txQueue = nullptr;
        return false;
    }

    txCallbackCountValue = 0;
    running = true;
    taskExited = false;

    const BaseType_t result = xTaskCreatePinnedToCore(
        txTaskEntry, "AudioI2STx", 2048, this,
        configMAX_PRIORITIES - 2, &txTask, AUDIO_PROCESSING_CORE);

    if (result != pdPASS) {
        running = false;
        taskExited = true;
        txTask = nullptr;
        AudioI2SPort::clearEventHandler(AudioI2SPort::Direction::Tx, this);
        AudioI2SPort::release(AudioI2SPort::Direction::Tx);
        txHandle = nullptr;
        vQueueDelete(txQueue);
        txQueue = nullptr;
        return false;
    }

    if (!AudioI2SPort::start(AudioI2SPort::Direction::Tx)) {
        running = false;
        xTaskNotifyGive(txTask);
        const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(200);
        while (txTask != nullptr &&
               static_cast<int32_t>(deadline - xTaskGetTickCount()) > 0) {
            vTaskDelay(1);
        }

        AudioI2SPort::clearEventHandler(AudioI2SPort::Direction::Tx, this);
        AudioI2SPort::stop(AudioI2SPort::Direction::Tx);
        AudioI2SPort::release(AudioI2SPort::Direction::Tx);
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
    if (!running && txQueue == nullptr && txHandle == nullptr) return;

    running = false;

    if (txTask != nullptr) {
        xTaskNotifyGive(txTask);
        const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(200);
        while (txTask != nullptr &&
               static_cast<int32_t>(deadline - xTaskGetTickCount()) > 0) {
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
        AudioI2SPort::clearEventHandler(AudioI2SPort::Direction::Tx, this);
        AudioI2SPort::stop(AudioI2SPort::Direction::Tx);
        AudioI2SPort::release(AudioI2SPort::Direction::Tx);
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

bool IRAM_ATTR AudioOutputI2S::onI2STransmit(i2s_chan_handle_t,
                                                                      i2s_event_data_t *,
                                                                      void *userContext) {
    auto *output = static_cast<AudioOutputI2S *>(userContext);
    if (output == nullptr || !output->running) return false;

    ++output->txCallbackCountValue;
    if (!AudioI2SPort::isClockOwner(AudioI2SPort::Direction::Tx)) return false;
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

    while (running) {

        if (xQueueReceive(txQueue, &pair, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }

        const int16_t *__restrict left = pair.left ? pair.left->data : nullptr;
        const int16_t *__restrict right = pair.right ? pair.right->data : nullptr;

        if (numChannels == 1) {
            // Mono: L/R mitteln und dasselbe Sample auf beide Slots legen.
             //TODO: Check this in godbolt. Is it optimized?
#pragma GCC unroll 4
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
            if (left && right) {
#pragma GCC unroll 4
                for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
                    const uint32_t l = static_cast<uint16_t>(left[i]);
                    const uint32_t r = static_cast<uint16_t>(right[i]);

                    buffer[i] = l | (r << 16);
                }
            } else if (left) {
#pragma GCC unroll 4
                for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
                    buffer[i] = static_cast<uint16_t>(left[i]);
                }
            } else if (right) {
#pragma GCC unroll 4
                for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
                    buffer[i] = static_cast<uint32_t>(static_cast<uint16_t>(right[i])) << 16;
                }
            } else {
                memset(buffer, 0, sizeof(buffer));
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
    }

    // Beim Beenden alle noch ausstehenden Blöcke freigeben.
    while (xQueueReceive(txQueue, &pair, 0) == pdTRUE) {
        releasePair(pair);
    }

    taskExited = true;
    txTask = nullptr;

    vTaskDelete(nullptr);
}
