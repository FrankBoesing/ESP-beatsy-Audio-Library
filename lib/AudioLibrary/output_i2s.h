#pragma once
#ifndef AUDIO_OUTPUT_I2S_H
#define AUDIO_OUTPUT_I2S_H

#include <Arduino.h>
#include <esp_attr.h>
#include <driver/i2s_std.h>
#include <driver/i2s_common.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "AudioStream.h"

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
 *   - 16-bit I2S slots
 *   - MCLK = 256 * sample rate
 *
 * The AudioStream blocks remain 16-bit. Conversion to the 32-bit
 * I2S slot format is performed only in the TX task.
 */
class AudioOutputI2S : public AudioStream {
  public:
    struct Pins {
        int8_t bclk;
        int8_t ws;
        int8_t dout;
        int8_t mclk; // -1 = disable
    };

    AudioOutputI2S();
    explicit AudioOutputI2S(const Pins &pins);
    ~AudioOutputI2S() override;

    bool begin();
    bool begin(const Pins &pins);
    void end();

    bool isRunning() const { return running; }
    uint32_t txCallbackCount() const { return txCallbackCountValue; }
    const Pins &pins() const { return i2sPins; }
    bool setSampleRate(float sampleRate);
    bool setChannelCount(uint channels); //Set to one if codec supports mono only (automatic stereo->mono conversion)

  protected:
    void update() override;
    bool beginHardware() override;

  private:
    struct BlockPair {
        audio_block_t *left;
        audio_block_t *right;
    };

    static constexpr size_t QUEUE_LENGTH = 4;

    // 6 descriptors x 128 frames x 4 bytes = 2 KiB (about 18 ms at 44.1 kHz).
    static constexpr uint16_t DMA_DESC_NUM = 6;
    static constexpr uint16_t DMA_FRAME_NUM = AUDIO_BLOCK_SAMPLES;

    Pins i2sPins = {-1, -1, -1, -1};
    uint numChannels = 2;

    i2s_chan_handle_t txHandle = nullptr;
    i2s_chan_config_t chanConfig = {};
    i2s_std_config_t stdConfig = {};

    QueueHandle_t txQueue = nullptr;
    TaskHandle_t txTask = nullptr;

    volatile bool running = false;
    volatile bool taskExited = true;
    volatile uint32_t txCallbackCountValue = 0;
    bool externalClockActive = false;

    static bool IRAM_ATTR onI2STransmit(i2s_chan_handle_t channel, i2s_event_data_t *event, void *userContext);
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
