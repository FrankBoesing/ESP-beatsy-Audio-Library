#pragma once
#ifndef AUDIO_I2S_PORT_H
#define AUDIO_I2S_PORT_H

#include <Arduino.h>
#include <esp_attr.h>
#include <driver/i2s_std.h>
#include <driver/i2s_common.h>

/*
 * Shared ESP-IDF full-duplex I2S port used by AudioInputI2S and AudioOutputI2S.
 * The first channel initialized is the clock master; the paired channel follows
 * its BCLK/WS. Only the selected event handler clocks the AudioStream graph.
 */
class AudioI2SPort {
  public:
    struct Pins {
        int8_t bclk;
        int8_t ws;
        int8_t dout;
        int8_t din;
        int8_t mclk;
    };

    enum class Direction : uint8_t { Tx, Rx, None };
    using EventCallback = bool (*)(i2s_chan_handle_t channel,
                                   i2s_event_data_t *event,
                                   void *userContext);

    static bool acquireTx(const Pins &pins, i2s_chan_handle_t *handle);
    static bool acquireRx(const Pins &pins, i2s_chan_handle_t *handle);
    static bool setEventHandler(Direction direction, EventCallback callback, void *userContext);
    static void clearEventHandler(Direction direction, void *userContext);
    static bool start(Direction direction);
    static void stop(Direction direction);
    static void release(Direction direction);
    static bool setSampleRate(float sampleRate);
    static bool IRAM_ATTR isClockOwner(Direction direction);
};

#endif
