#pragma once
#ifndef FILTER_DCBLOCK_H
#define FILTER_DCBLOCK_H

#include "AudioStream.h"

#include <stdint.h>

class AudioFilterDCBlock : public AudioStream {
  public:
    AudioFilterDCBlock();

    // First-order high-pass filter for removing DC offset.
    // The default cutoff frequency is 10 Hz.
    void frequency(float frequency);

    void update() override;

  private:
    audio_block_t *inputQueueArray[1] = {};

    // Configuration is set from control/task context and read by update().
    portMUX_TYPE _configMux = portMUX_INITIALIZER_UNLOCKED;
    float _cutoffFrequency = 10.0f;

    // Processing state belongs to the audio task.
    int32_t _previousInput = 0;
    int32_t _previousOutput = 0;
};

#endif
