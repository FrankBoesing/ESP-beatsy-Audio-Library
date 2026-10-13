#pragma once
#ifndef AUDIO_INPUT_I2S_H
#define AUDIO_INPUT_I2S_H

#include <Arduino.h>
#include <atomic>
#include "AudioStream.h"
#include "i2s_port.h"

/*
 * ESP32 implementation of the Teensy-compatible AudioInputI2S source.
 * Output 0 is the left channel; output 1 is the right channel.
 *
 * The ESP-IDF-specific transport is private to this implementation.
 * AudioStream blocks and graph semantics remain Teensy-compatible.
 */
class AudioInputI2S : public AudioStream {
  public:
    struct Pins {
        int8_t bclk;
        int8_t ws;
        int8_t din;
        int8_t mclk;
        uint8_t controller = 0; // ESP-IDF hardware controller index (0..3)
    };

    // Reference pinout for the classic ESP32 Audio Kit. ESP32-S3 boards
    // should pass the actual wiring explicitly.
    static constexpr Pins defaultPins() {
#if defined(CONFIG_IDF_TARGET_ESP32)
        return {27, 25, 35, 0};
#else
        return {-1, -1, -1, -1};
#endif
    }

    AudioInputI2S();
    explicit AudioInputI2S(const Pins &pins);
    ~AudioInputI2S() override;

    bool begin();
    bool begin(const Pins &pins);
    void end();

    bool isRunning() const;
    uint32_t rxBlockCount() const;
    uint32_t rxErrorCount() const;
    uint32_t droppedBlockCount() const;
    const Pins &pins() const { return i2sPins; }

  protected:
    void update() override;
    bool beginHardware() override;

  private:
    // One I2S DMA callback carries one AudioStream block. A small SPSC ring
    // absorbs brief scheduler delays; if full, the ISR drops the newest block.
    static constexpr uint32_t RX_RING_BLOCKS = 4;
    static_assert((RX_RING_BLOCKS & (RX_RING_BLOCKS - 1U)) == 0, "RX ring blocks must be a power of two");

    Pins i2sPins = defaultPins();
    i2s_chan_handle_t rxHandle = nullptr;

    alignas(4) int16_t rxSampleRing[RX_RING_BLOCKS][AUDIO_BLOCK_SAMPLES * 2] = {};
    // Reject queued samples captured under a previous global I2S sample rate.
    uint32_t rxRateGeneration[RX_RING_BLOCKS] = {};
    std::atomic<uint32_t> writeBlockIndex{0};
    std::atomic<uint32_t> readBlockIndex{0};

    std::atomic<uint32_t> running{0};
    std::atomic<uint32_t> rxBlockCountValue{0};
    std::atomic<uint32_t> rxErrorCountValue{0};
    std::atomic<uint32_t> droppedBlockCountValue{0};

    static bool AUDIO_I2S_ISR_ATTR onI2SReceive(i2s_chan_handle_t channel, i2s_event_data_t *event, void *userContext);
    bool beginInternal();

    AudioInputI2S(const AudioInputI2S &) = delete;
    AudioInputI2S &operator=(const AudioInputI2S &) = delete;
};

#endif
