/*
 * Audio Library for Teensy 3.X
 * Copyright (c) 2016, Paul Stoffregen, paul@pjrc.com
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
#ifndef synth_karplusstrong_h_
#define synth_karplusstrong_h_

#include "AudioStream.h"

#include <stdint.h>

class AudioSynthKarplusStrong : public AudioStream {
  public:
    AudioSynthKarplusStrong();

    // Generate a plucked-string sound. velocity is normalized to 0..1.
    void noteOn(float frequency, float velocity);

    // Stop the current string vibration immediately. velocity is retained
    // for source compatibility with the original Teensy API.
    void noteOff(float velocity);

    void update() override;

  private:
    static constexpr uint16_t MAX_BUFFER_LENGTH = 536;

    // State: 0 = stopped, 1 = initialize a new excitation, 2 = playing.
    uint8_t state = 0;
    uint16_t bufferLen = 0;
    uint16_t bufferIndex = 0;
    int32_t magnitude = 0;
    static uint32_t seed;
    int16_t buffer[MAX_BUFFER_LENGTH] = {};

    // noteOn()/noteOff() can be called outside the audio update task.
    portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
    uint32_t stateGeneration = 0;
};

#endif
