/*
 * Audio Library for Teensy 3.X
 * Copyright (c) 2014, Pete (El Supremo)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef synth_tonesweep_h_
#define synth_tonesweep_h_

#include "AudioStream.h"

#include <stdint.h>

// Frequency-swept sine oscillator. The sweep is sent to output 0 (left).
class AudioSynthToneSweep : public AudioStream {
  public:
    AudioSynthToneSweep() : AudioStream(0, nullptr) {}

    // Start a linear frequency sweep. Returns false for invalid parameters.
    // Amplitude must be in [0, 1], frequencies below Nyquist, and time > 0.
    bool play(float t_amp, int t_lo, int t_hi, float t_time);

    void update() override;

    unsigned char isPlaying();

    // Compatibility with the Teensy API: read() returns the current integer Hz.
    float read() {
        uint64_t frequency;
        unsigned char busy;

        portENTER_CRITICAL(&stateMux);
        frequency = tone_freq;
        busy = sweep_busy;
        portEXIT_CRITICAL(&stateMux);

        if (!busy) {
            return 0.0f;
        }
        return (float)(frequency >> 32);
    }

  private:
    portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
    uint32_t stateGeneration = 0;

    int16_t tone_amp = 0;
    uint64_t tone_freq = 0;       // Q32.32 Hz
    uint64_t tone_target = 0;     // Q32.32 Hz
    uint32_t tone_phase = 0;      // Q0.32 turns
    uint64_t tone_incr = 0;       // whole Q32.32 Hz steps per output sample
    uint64_t tone_remainder = 0;  // fractional step numerator
    uint64_t remainder_accum = 0;
    uint64_t sweep_samples = 0;
    uint64_t samples_elapsed = 0;
    int tone_sign = 1;
    unsigned char sweep_busy = 0;
};

#endif
