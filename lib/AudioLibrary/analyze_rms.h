
/*
 * Audio Library for Teensy 3.X
 * Copyright (c) 2014, Paul Stoffregen
 *
 * Adapted for the ESP32 Audio Library.
 *
 * Development of the original audio library was funded by PJRC.COM, LLC.
 * The original license permits modification and redistribution provided
 * its copyright notice and license are retained.
 */

#ifndef analyze_rms_h_
#define analyze_rms_h_

#include <Arduino.h>
#include "AudioStream.h"

class AudioAnalyzeRMS : public AudioStream {
  public:
    AudioAnalyzeRMS(void) : AudioStream(1, inputQueueArray), accum(0), count(0) {}

    // True when at least one audio update has been accumulated.
    bool available(void);

    // Return RMS level normalized approximately to [-1.0, 1.0].
    // The accumulated measurement is reset after reading.
    float read(void);
    void update(void) override;

  private:
    audio_block_t *inputQueueArray[1];
    int64_t accum;
    uint32_t count;
};

#endif // analyze_rms_h_
