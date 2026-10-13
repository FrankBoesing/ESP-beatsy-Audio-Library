#pragma once
#ifndef AUDIO_I2S_PORT_H
#define AUDIO_I2S_PORT_H

#include <Arduino.h>
#include <esp_attr.h>
#include <driver/i2s_std.h>
#include <driver/i2s_common.h>
#include <freertos/FreeRTOS.h>

#if defined(CONFIG_I2S_ISR_IRAM_SAFE) && CONFIG_I2S_ISR_IRAM_SAFE
#define AUDIO_I2S_ISR_ATTR IRAM_ATTR
#else
#define AUDIO_I2S_ISR_ATTR
#endif

/*
 * Owns one physical ESP-IDF I2S controller and its optional TX/RX pair.
 *
 * Four software controller IDs (0..3) are reserved. The SoC/ESP-IDF driver
 * decides which IDs physically exist; unsupported IDs fail in i2s_new_channel.
 *
 * Controller lifetime is separate from wire format. Standard stereo PCM,
 * software-encoded SPDIF_BMC TX and hardware PCM-to-PDM DAC TX are implemented.
 * TDM and SPDIF RX decoding are not implemented.
 */
class AudioI2SPort {
  public:
    static constexpr uint8_t MAX_CONTROLLERS = 4;

    struct Pins {
        int8_t bclk;
        int8_t ws;
        int8_t dout;
        int8_t din;
        int8_t mclk;
        uint8_t controller = 0; // Hardware index; default preserves I2S0 behavior.
        int8_t dout2 = -1;      // Optional second PDM-DAC data line.
    };

    enum class Direction : uint8_t { Tx, Rx, None };
    enum class PeripheralMode : uint8_t { Standard, TDM, PDM };
    enum class DataEncoding : uint8_t { PCM, SpdifBMC };

    struct Format {
        PeripheralMode mode = PeripheralMode::Standard;
        DataEncoding encoding = DataEncoding::PCM;
        uint8_t slots = 2;
        uint8_t slotBits = 16;
        // Physical serializer rate relative to the AudioStream sample rate.
        // SPDIF BMC uses 4x; PDM PCM-to-PDM uses the source sample rate directly.
        uint8_t rateMultiplier = 1;
    };

    using EventCallback = bool (*)(i2s_chan_handle_t channel,
                                   i2s_event_data_t *event,
                                   void *userContext);

    static bool acquireTx(const Pins &pins, i2s_chan_handle_t *handle);
    static bool acquireTx(const Pins &pins, i2s_chan_handle_t *handle, const Format &format);
    static bool acquireRx(const Pins &pins, i2s_chan_handle_t *handle);
    static bool acquireRx(const Pins &pins, i2s_chan_handle_t *handle, const Format &format);

    static bool setEventHandler(uint8_t controller, Direction direction,
                                EventCallback callback, void *userContext);
    static void clearEventHandler(uint8_t controller, Direction direction, void *userContext);
    static bool start(uint8_t controller, Direction direction);
    // False means the channel could not be safely disabled. In that case the
    // caller must retain the port handle/resources and may retry end().
    static bool stop(uint8_t controller, Direction direction);
    // False indicates that channel resources could not be released; the caller
    // must retain its handle and may retry cleanup.
    static bool release(uint8_t controller, Direction direction);

    // AudioStream has one global sample rate, so all allocated controllers
    // are reconfigured together.
    static bool setSampleRate(float sampleRate);
    static bool AUDIO_I2S_ISR_ATTR isClockOwner(uint8_t controller, Direction direction);
};

#endif
