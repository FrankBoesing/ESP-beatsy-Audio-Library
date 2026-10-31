#pragma once
#ifndef AUDIO_OUTPUT_I2S2_H
#define AUDIO_OUTPUT_I2S2_H

#include "output_i2s.h"

// Convenience class for the second hardware I2S controller (I2S_NUM_1).
// Pin assignments must be supplied for the target board.
class AudioOutputI2S2 : public AudioOutputI2S {
  public:
    using Pins = AudioOutputI2S::Pins;
    static constexpr uint8_t CONTROLLER = 1;

    AudioOutputI2S2();
    explicit AudioOutputI2S2(const Pins &pins);

    bool begin();
    bool begin(const Pins &pins);

  private:
    static Pins selectController(Pins pins) {
        pins.controller = CONTROLLER;
        return pins;
    }
};

#endif
