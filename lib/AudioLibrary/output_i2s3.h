#pragma once
#ifndef AUDIO_OUTPUT_I2S3_H
#define AUDIO_OUTPUT_I2S3_H

#include "output_i2s.h"

// Convenience class for the third hardware I2S controller (I2S_NUM_2).
// This is available only on SoCs that implement a third I2S controller.
class AudioOutputI2S3 : public AudioOutputI2S {
  public:
    using Pins = AudioOutputI2S::Pins;
    static constexpr uint8_t CONTROLLER = 2;

    AudioOutputI2S3();
    explicit AudioOutputI2S3(const Pins &pins);

    bool begin();
    bool begin(const Pins &pins);

  private:
    static Pins selectController(Pins pins) {
        pins.controller = CONTROLLER;
        return pins;
    }
};

#endif
