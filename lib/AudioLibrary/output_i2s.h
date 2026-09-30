#pragma once
#ifndef AUDIO_OUTPUT_I2S_H
#define AUDIO_OUTPUT_I2S_H

#include <Arduino.h>
#include <esp_attr.h>
#include <driver/i2s_std.h>
#include <driver/i2s_common.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "softcodecs/AudioStream.h"

/*
 * ESP32 Audio Core - I2S output
 *
 * ESP32-specific hardware implementation of the Teensy-compatible
 * AudioOutputI2S API.
 *
 * The transport uses the ESP-IDF standard I2S driver directly.
 *
 * Format:
 *   - I2S Philips standard
 *   - master
 *   - stereo
 *   - 32-bit I2S slots
 *   - MCLK = 256 * sample rate
 *
 * The AudioStream blocks remain 16-bit. Conversion to the 32-bit
 * I2S slot format is performed only in the TX task.
 */
class AudioOutputI2S : public AudioStream
{
public:
    struct Pins
    {
        int8_t bclk;
        int8_t ws;
        int8_t dout;
        int8_t mclk;
    };

    /*
     * ESP32 Audio Kit V2.2 / ES8388 reference pinout:
     *   BCLK  = GPIO27
     *   WS    = GPIO25
     *   DOUT  = GPIO26
     *   MCLK  = GPIO0
     *
     * These are board-specific defaults and can be overridden with
     * AudioOutputI2S(Pins) or begin(Pins).
     */
    static constexpr Pins defaultPins()
    {
#if defined(CONFIG_IDF_TARGET_ESP32)
        return {27, 25, 26, 0};
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
        // No universal hardware pin assignment on ESP32-S3.
        return {4, 5, 21, -1};
#else
        return {-1, -1, -1, -1};
#endif
    }

    AudioOutputI2S();
    explicit AudioOutputI2S(const Pins &pins);
    ~AudioOutputI2S() override;

    bool begin();
    bool begin(const Pins &pins);
    void end();

    bool isRunning() const { return running; }
    uint32_t txCallbackCount() const { return txCallbackCountValue; }
    const Pins &pins() const { return i2sPins; }

protected:
    void update() override;
    bool beginHardware() override;

private:
    struct BlockPair
    {
        audio_block_t *left;
        audio_block_t *right;
    };

    static constexpr size_t QUEUE_LENGTH = 4;

    // Same DMA sizing as the reference ESP32-audioI2S configuration.
    static constexpr uint16_t DMA_DESC_NUM  = 16;
    static constexpr uint16_t DMA_FRAME_NUM = AUDIO_BLOCK_SAMPLES;

    Pins i2sPins = defaultPins();

    i2s_chan_handle_t txHandle = nullptr;
    i2s_chan_config_t chanConfig = {};
    i2s_std_config_t stdConfig = {};

    QueueHandle_t txQueue = nullptr;
    TaskHandle_t txTask = nullptr;

    volatile bool running = false;
    volatile bool taskExited = true;
    volatile uint32_t txCallbackCountValue = 0;
    bool externalClockActive = false;

    static bool IRAM_ATTR onI2STransmit(i2s_chan_handle_t channel,
                                        i2s_event_data_t *event,
                                        void *userContext);
    static void txTaskEntry(void *arg);
    void txTaskLoop();

    bool beginInternal();
    void releasePair(BlockPair &pair);

    // Non-copyable
    AudioOutputI2S(const AudioOutputI2S &) = delete;
    AudioOutputI2S &operator=(const AudioOutputI2S &) = delete;

    audio_block_t *inputQueueArray[2] = {};
};

#endif
