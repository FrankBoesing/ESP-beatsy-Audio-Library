#include "input_i2s2.h"

AudioInputI2S2::AudioInputI2S2()
    : AudioInputI2S(Pins{-1, -1, -1, -1, CONTROLLER}) {}

AudioInputI2S2::AudioInputI2S2(const Pins &pins)
    : AudioInputI2S(selectController(pins)) {}

bool AudioInputI2S2::begin() {
    return AudioInputI2S::begin();
}

bool AudioInputI2S2::begin(const Pins &pins) {
    return AudioInputI2S::begin(selectController(pins));
}
