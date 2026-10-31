#include "input_i2s3.h"

AudioInputI2S3::AudioInputI2S3()
    : AudioInputI2S(Pins{-1, -1, -1, -1, CONTROLLER}) {}

AudioInputI2S3::AudioInputI2S3(const Pins &pins)
    : AudioInputI2S(selectController(pins)) {}

bool AudioInputI2S3::begin() {
    return AudioInputI2S::begin();
}

bool AudioInputI2S3::begin(const Pins &pins) {
    return AudioInputI2S::begin(selectController(pins));
}
