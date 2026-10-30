#include "output_i2s3.h"

AudioOutputI2S3::AudioOutputI2S3()
    : AudioOutputI2S(Pins{-1, -1, -1, -1, CONTROLLER}) {}

AudioOutputI2S3::AudioOutputI2S3(const Pins &pins)
    : AudioOutputI2S(selectController(pins)) {}

bool AudioOutputI2S3::begin() {
    return AudioOutputI2S::begin();
}

bool AudioOutputI2S3::begin(const Pins &pins) {
    return AudioOutputI2S::begin(selectController(pins));
}
