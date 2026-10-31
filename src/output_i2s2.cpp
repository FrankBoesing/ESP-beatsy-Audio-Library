#include "output_i2s2.h"

AudioOutputI2S2::AudioOutputI2S2()
    : AudioOutputI2S(Pins{-1, -1, -1, -1, CONTROLLER}) {}

AudioOutputI2S2::AudioOutputI2S2(const Pins &pins)
    : AudioOutputI2S(selectController(pins)) {}

bool AudioOutputI2S2::begin() {
    return AudioOutputI2S::begin();
}

bool AudioOutputI2S2::begin(const Pins &pins) {
    return AudioOutputI2S::begin(selectController(pins));
}
