#include "output_i2s.h"

#include <cstring>
#include <esp_err.h>

AudioOutputI2S::AudioOutputI2S() : AudioStream(2, inputQueueArray) {
    // kein Hardware-Init hier
}

AudioOutputI2S::AudioOutputI2S(const Pins &pins) : AudioStream(2, inputQueueArray), i2sPins(pins) {
    // kein Hardware-Init hier
}

AudioOutputI2S::~AudioOutputI2S() { end(); }

bool AudioOutputI2S::begin() { return beginInternal(); }

bool AudioOutputI2S::begin(const Pins &pins) {
    end();
    i2sPins = pins;
    return beginInternal();
}

bool AudioOutputI2S::beginHardware() { return beginInternal(); }

OSIZE
bool AudioOutputI2S::setSampleRate(float hz) {
    if (hz <= 0.0f) return false;

    if (!running || txHandle == nullptr) {
        // I2S noch nicht gestartet.
        return AudioStream::setSampleRate(hz);
    }

    if (hz == AudioStream::sampleRate()) return true;

    // Queue leeren
    BlockPair pair;
    while (xQueueReceive(txQueue, &pair, 0) == pdTRUE) {
        releasePair(pair);
    }

    esp_err_t err = i2s_channel_disable(txHandle);
    if (err != ESP_OK) return false;

    i2s_std_clk_config_t clkConfig = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)std::lround(hz));
    clkConfig.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    err = i2s_channel_reconfig_std_clock(txHandle, &clkConfig);
    if (err != ESP_OK) return false;

    err = i2s_channel_enable(txHandle);
    if (err != ESP_OK) return false;

    return AudioStream::setSampleRate(hz);
}

OSIZE
bool AudioOutputI2S::beginInternal() {
    if (running) {
        return true;
    }

    if (i2sPins.bclk < 0 || i2sPins.ws < 0 || i2sPins.dout < 0) {
        return false;
    }

    txQueue = xQueueCreate(QUEUE_LENGTH, sizeof(BlockPair));
    if (txQueue == nullptr) {
        return false;
    }

    /*
     * ESP-IDF I2S channel configuration.
     *
     * This follows the architecture used by ESP32-audioI2S:
     * master controller, explicit DMA sizing, automatic zero output
     * when no data is available.
     */
    memset(&chanConfig, 0, sizeof(chanConfig));
    chanConfig.id = I2S_NUM_AUTO;
    chanConfig.role = I2S_ROLE_MASTER;
    chanConfig.dma_desc_num = DMA_DESC_NUM;
    chanConfig.dma_frame_num = DMA_FRAME_NUM;
    chanConfig.auto_clear_after_cb = true;
    chanConfig.allow_pd = false;
    chanConfig.intr_priority = 2;

    esp_err_t err = i2s_new_channel(&chanConfig, &txHandle, nullptr);

    if (err != ESP_OK) {
        txHandle = nullptr;
        vQueueDelete(txQueue);
        txQueue = nullptr;
        return false;
    }

    /*
     * Standard Philips I2S.
     *
     * IMPORTANT:
     * The ES8388 reference implementation uses 32-bit slots here,
     * even though our AudioStream blocks contain 16-bit PCM.
     */
    memset(&stdConfig, 0, sizeof(stdConfig));

    stdConfig.clk_cfg.sample_rate_hz = static_cast<uint32_t>(AudioStream::sampleRate());
    stdConfig.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
    stdConfig.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    stdConfig.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);

    stdConfig.gpio_cfg.bclk = static_cast<gpio_num_t>(i2sPins.bclk);
    stdConfig.gpio_cfg.ws = static_cast<gpio_num_t>(i2sPins.ws);
    stdConfig.gpio_cfg.dout = static_cast<gpio_num_t>(i2sPins.dout);
    stdConfig.gpio_cfg.din = I2S_GPIO_UNUSED;

    // MCLK-Pin dynamsich aus i2sPins zuweisen (GPIO 0 bei ESP32)
    stdConfig.gpio_cfg.mclk = (i2sPins.mclk >= 0) ? static_cast<gpio_num_t>(i2sPins.mclk) : I2S_GPIO_UNUSED;

    stdConfig.gpio_cfg.invert_flags.mclk_inv = false;
    stdConfig.gpio_cfg.invert_flags.bclk_inv = false;
    stdConfig.gpio_cfg.invert_flags.ws_inv = false;

    err = i2s_channel_init_std_mode(txHandle, &stdConfig);
    if (err != ESP_OK) {
        i2s_del_channel(txHandle);
        txHandle = nullptr;
        vQueueDelete(txQueue);
        txQueue = nullptr;
        return false;
    }

    i2s_event_callbacks_t callbacks = {};
    callbacks.on_sent = &AudioOutputI2S::onI2STransmit;
    err = i2s_channel_register_event_callback(txHandle, &callbacks, this);
    if (err != ESP_OK) {
        i2s_del_channel(txHandle);
        txHandle = nullptr;
        vQueueDelete(txQueue);
        txQueue = nullptr;
        return false;
    }

    if (!AudioStream::setExternalUpdateClock(true)) {
        i2s_del_channel(txHandle);
        txHandle = nullptr;
        vQueueDelete(txQueue);
        txQueue = nullptr;
        return false;
    }
    externalClockActive = true;
    txCallbackCountValue = 0;

    running = true;
    taskExited = false;

    BaseType_t result = xTaskCreatePinnedToCore(txTaskEntry, "AudioI2STx", 2048, this, configMAX_PRIORITIES - 2,
                                                &txTask, AUDIO_PROCESSING_CORE);

    if (result != pdPASS) {
        running = false;
        taskExited = true;
        AudioStream::setExternalUpdateClock(false);
        externalClockActive = false;

        i2s_del_channel(txHandle);
        txHandle = nullptr;

        vQueueDelete(txQueue);
        txQueue = nullptr;
        txTask = nullptr;

        return false;
    }

    err = i2s_channel_enable(txHandle);
    if (err != ESP_OK) {
        running = false;
        xTaskNotifyGive(txTask);

        const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(200);
        while (txTask != nullptr && static_cast<int32_t>(deadline - xTaskGetTickCount()) > 0) {
            vTaskDelay(1);
        }

        AudioStream::setExternalUpdateClock(false);
        externalClockActive = false;
        i2s_del_channel(txHandle);
        txHandle = nullptr;
        vQueueDelete(txQueue);
        txQueue = nullptr;
        return false;
    }

    return true;
}

OSIZE
void AudioOutputI2S::end() {
    if (!running && txQueue == nullptr && txHandle == nullptr && !externalClockActive) {
        return;
    }

    running = false;

    if (txTask != nullptr) {
        xTaskNotifyGive(txTask);

        /*
         * Give the TX task enough time to leave i2s_channel_write(),
         * release its current block and delete itself.
         */
        const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(200);

        while (txTask != nullptr && static_cast<int32_t>(deadline - xTaskGetTickCount()) > 0) {
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
        i2s_channel_disable(txHandle);
        i2s_del_channel(txHandle);
        txHandle = nullptr;
    }

    if (txQueue != nullptr) {
        vQueueDelete(txQueue);
        txQueue = nullptr;
    }

    if (externalClockActive) {
        AudioStream::setExternalUpdateClock(false);
        externalClockActive = false;
    }
}

OSPEED
void AudioOutputI2S::update() {
    BlockPair pair;

    pair.left = receiveReadOnly(0);
    pair.right = receiveReadOnly(1);

    if (pair.left == nullptr && pair.right == nullptr) {
        return;
    }

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

bool IRAM_ATTR AudioOutputI2S::onI2STransmit(i2s_chan_handle_t, i2s_event_data_t *, void *userContext) {
    auto *output = static_cast<AudioOutputI2S *>(userContext);
    if (output == nullptr || !output->running) {
        return false;
    }

    output->txCallbackCountValue = output->txCallbackCountValue + 1;
    return AudioStream::update_all_from_isr();
}

#if 1
OSPEED
void AudioOutputI2S::txTaskLoop() {
    static uint32_t writeErrorCount = 0;
    static uint32_t lastErrorLogMs = 0;
    /*
     * One stereo audio block contains:
     *
     *   AUDIO_BLOCK_SAMPLES * 2 channels
     *
     */
    uint16_t buffer[AUDIO_BLOCK_SAMPLES * 2];

    while (running) {
        BlockPair pair;

        if (xQueueReceive(txQueue, &pair, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }

        if (pair.left->data != nullptr && pair.right->data != nullptr) {
            const uint16_t *__restrict left = (uint16_t *)pair.left->data;
            const uint16_t *__restrict right = (uint16_t *)pair.right->data;
            uint32_t *__restrict dst = (uint32_t *)buffer;
#pragma GCC unroll 4
            for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
                const uint32_t l = left[i];
                const uint32_t r = right[i];
                dst[i] = l | (r << 16);
            }
        } else
            //Left
            if (pair.left->data != nullptr) {
                const uint16_t *__restrict left = (uint16_t *)pair.left->data;
                uint32_t *__restrict dst = (uint32_t *)buffer;
#pragma GCC unroll 4
                for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
                    const uint32_t l = left[i];
                    dst[i] = l | (0 << 16);
                }
            } else
                //Right
                if (pair.right->data != nullptr) {
                    const uint16_t *__restrict right = (uint16_t *)pair.right->data;
                    uint32_t *__restrict dst = (uint32_t *)buffer;
#pragma GCC unroll 4
                    for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
                        const uint32_t r = right[i];
                        dst[i] = 0 | (r << 16);
                    }
                } else
                //None
                {
                    memset(buffer, 0, sizeof(buffer));
                }

        size_t bytesWritten = 0;

        /*
         * The blocking call is deliberately isolated in this task.
         * A timeout prevents shutdown from becoming permanently stuck
         * if the I2S driver stops accepting data.
         */
        const esp_err_t err = i2s_channel_write(txHandle, buffer, sizeof(buffer), &bytesWritten, 100);

        if (err != ESP_OK || bytesWritten != sizeof(buffer)) {
            const uint32_t now = millis();
            ++writeErrorCount;

            if (lastErrorLogMs == 0 || now - lastErrorLogMs >= 1000) {
                ESP_LOGE("I2S", "TX error #%lu: %s, bytes=%lu/%lu\n", writeErrorCount, esp_err_to_name(err),
                         bytesWritten, sizeof(buffer));
                lastErrorLogMs = now;
            }
        }
        releasePair(pair);
    }

    /*
     * Release anything still queued during shutdown.
     */
    BlockPair pair;

    while (xQueueReceive(txQueue, &pair, 0) == pdTRUE) {
        releasePair(pair);
    }

    taskExited = true;
    txTask = nullptr;

    vTaskDelete(nullptr);
}
#else
OSPEED
void AudioOutputI2S::txTaskLoop() {
    static uint32_t writeErrorCount = 0;
    static uint32_t lastErrorLogMs = 0;

    /*
     * One stereo audio block contains:
     *
     *   AUDIO_BLOCK_SAMPLES * 2 channels
     *
     * The AudioStream samples are 16-bit. The samples are interleaved
     * as L/R pairs in one contiguous buffer.
     *
     * alignas(4) guarantees the alignment required for the 32-bit
     * stores used in the optimized stereo path below.
     */
    alignas(4) int16_t buffer[AUDIO_BLOCK_SAMPLES * 2];

    while (running) {
        BlockPair pair;

        if (xQueueReceive(txQueue, &pair, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }

        if (pair.left != nullptr && pair.right != nullptr) {
            /*
             * Optimized stereo interleave.
             *
             * Local __restrict pointers tell the compiler that the
             * source buffers and destination buffer do not overlap.
             *
             * The 32-bit store writes one complete L/R pair at once.
             *
             * Measured on ESP32 @ 240 MHz:
             *   no unroll:       ~10.23 cycles/sample
             *   manual 4x:        ~9.48 cycles/sample
             *   GCC unroll 4:     ~7.48 cycles/sample
             */
            const int16_t *__restrict left = pair.left->data;
            const int16_t *__restrict right = pair.right->data;
            uint32_t *__restrict dst = reinterpret_cast<uint32_t *>(buffer);

#pragma GCC unroll 4
            for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
                const uint32_t l = static_cast<uint16_t>(left[i]);
                const uint32_t r = static_cast<uint16_t>(right[i]);

                dst[i] = l | (r << 16);
            }

        } else if (pair.left != nullptr) {
            /*
             * LEFT only:
             * Clear the whole stereo buffer first, then fill LEFT.
             */
            memset(buffer, 0, sizeof(buffer));

            const int16_t *__restrict left = pair.left->data;
            int16_t *__restrict dst = buffer;

#pragma GCC unroll 4
            for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
                dst[2 * i] = left[i];
            }

        } else if (pair.right != nullptr) {
            /*
             * RIGHT only:
             * Clear the whole stereo buffer first, then fill RIGHT.
             */
            memset(buffer, 0, sizeof(buffer));

            const int16_t *__restrict right = pair.right->data;
            int16_t *__restrict dst = buffer + 1;

#pragma GCC unroll 4
            for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
                dst[2 * i] = right[i];
            }

        } else {
            // Beide nullptr: kompletter Speicher-Reset
            memset(buffer, 0, sizeof(buffer));
        }

        size_t bytesWritten = 0;

        /*
         * The blocking call is deliberately isolated in this task.
         * A timeout prevents shutdown from becoming permanently stuck
         * if the I2S driver stops accepting data.
         */
        const esp_err_t err = i2s_channel_write(txHandle, buffer, sizeof(buffer), &bytesWritten, 100);

        if (err != ESP_OK || bytesWritten != sizeof(buffer)) {
            const uint32_t now = millis();
            ++writeErrorCount;

            if (lastErrorLogMs == 0 || now - lastErrorLogMs >= 1000) {
                ESP_LOGE("I2S", "TX error #%lu: %s, bytes=%lu/%lu\n", writeErrorCount, esp_err_to_name(err),
                         bytesWritten, sizeof(buffer));

                lastErrorLogMs = now;
            }
        }

        releasePair(pair);
    }

    /*
     * Release anything still queued during shutdown.
     */
    BlockPair pair;

    while (xQueueReceive(txQueue, &pair, 0) == pdTRUE) {
        releasePair(pair);
    }

    taskExited = true;
    txTask = nullptr;

    vTaskDelete(nullptr);
}
#endif
