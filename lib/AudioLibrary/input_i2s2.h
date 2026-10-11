#pragma once
#ifndef AUDIO_INPUT_I2S2_H
#define AUDIO_INPUT_I2S2_H

#include "input_i2s.h"

// Convenience class for the second hardware I2S controller (I2S_NUM_1).
// Pin assignments must be supplied for the target board.
class AudioInputI2S2 : public AudioInputI2S {
  public:
    using Pins = AudioInputI2S::Pins;
    static constexpr uint8_t CONTROLLER = 1;

    AudioInputI2S2();
    explicit AudioInputI2S2(const Pins &pins);

    bool begin();
    bool begin(const Pins &pins);

  private:
    static Pins selectController(Pins pins) {
        pins.controller = CONTROLLER;
        return pins;
    }
};

#endif
