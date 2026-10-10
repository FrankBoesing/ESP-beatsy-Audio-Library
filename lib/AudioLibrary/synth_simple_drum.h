/* Audio Library for Teensy 3.X
 * Copyright (c) 2016, Byron Jacquot, SparkFun Electronics
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

#pragma once
#ifndef _SYNTH_SIMPLE_DRUM_H_
#define _SYNTH_SIMPLE_DRUM_H_

#include "AudioStream.h"

#include <stdint.h>

class AudioSynthSimpleDrum : public AudioStream {
  public:
    AudioSynthSimpleDrum();

    void noteOn();

    void frequency(float freq);
    void length(int32_t milliseconds);
    void secondMix(float level);
    void pitchMod(float depth);

    // Retain the original Teensy API.
    using AudioStream::release;

    void update() override;

  private:
    audio_block_t *inputQueueArray[1] = {};

    // Envelope and oscillator state; update() owns these apart from noteOn().
    int32_t env_lin_current = 0;
    uint32_t wav_phasor = 0;
    uint32_t wav_phasor2 = 0;
    uint32_t noteGeneration = 0;

    // Configuration is changed by public setters and sampled once per block.
    int32_t env_decrement = 0;
    uint32_t wav_increment = 0;
    int16_t wav_amplitude1 = 0x7fff;
    int16_t wav_amplitude2 = 0;
    int32_t wav_pitch_mod = 0;

    portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
    portMUX_TYPE configMux = portMUX_INITIALIZER_UNLOCKED;
};

#endif
