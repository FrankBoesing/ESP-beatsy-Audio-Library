/*
 * Audio Library for Teensy 3.X
 * Copyright (c) 2014, Paul Stoffregen, paul@pjrc.com
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

#ifndef filter_variable_h_
#define filter_variable_h_

#include "AudioStream.h"

#include <stdint.h>

class AudioFilterStateVariable : public AudioStream {
  public:
    AudioFilterStateVariable();

    void frequency(float freq);
    void resonance(float q);
    void octaveControl(float octaves);
    void update() override;

  private:
    struct Parameters {
        int32_t fcenter;
        int32_t fmult;
        int32_t octaveMult;
        int32_t damping;
    };

    void update_fixed(const int16_t *input, int16_t *lowpass,
                      int16_t *bandpass, int16_t *highpass,
                      const Parameters &parameters);
    void update_variable(const int16_t *input, const int16_t *control,
                         int16_t *lowpass, int16_t *bandpass,
                         int16_t *highpass, const Parameters &parameters);

    audio_block_t *inputQueueArray[2] = {};

    // Filter state is accessed only by update(), not by the parameter setters.
    int32_t state_inputprev = 0;
    int32_t state_lowpass = 0;
    int32_t state_bandpass = 0;

    // Setters may be called from another task while update() is running.
    portMUX_TYPE settingsMux = portMUX_INITIALIZER_UNLOCKED;
    Parameters settings = {};
};

#endif
