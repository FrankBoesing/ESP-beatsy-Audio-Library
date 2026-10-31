/*
 * Audio Library for Teensy 3.X
 * Copyright (c) 2017, Paul Stoffregen, paul@pjrc.com
 *
 * Development of this audio library was funded by PJRC.COM, LLC by sales of
 * Teensy and Audio Adaptor boards. Please support PJRC's efforts to develop
 * open source software by purchasing Teensy or other PJRC products.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice, development funding notice, and this permission
 * notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef synth_pwm_h_
#define synth_pwm_h_

#include "AudioStream.h"

#include <stdint.h>

class AudioSynthWaveformPWM : public AudioStream {
  public:
    AudioSynthWaveformPWM();

    void frequency(float freq);
    void amplitude(float n);
    void update() override;

  private:
    audio_block_t *inputQueueArray[1] = {};

    // Q16.16 samples per half-cycle. The default frequency is initialized
    // in the constructor, so update() never reads an uninitialized duration.
    uint32_t duration = 0;

    // Configuration setters can run outside the audio task.
    portMUX_TYPE configMux = portMUX_INITIALIZER_UNLOCKED;
    int32_t requestedMagnitude = 0;
    uint32_t amplitudeGeneration = 0;

    // These fields are owned by update().
    int32_t magnitude = 0;
    uint32_t elapsed = 0;
    uint32_t appliedAmplitudeGeneration = 0;
};

#endif
