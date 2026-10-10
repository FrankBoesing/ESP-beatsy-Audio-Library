
/*
 * Audio Library for Teensy 3.X
 * Copyright (c) 2014, Jonathan Payne (jon@jonnypayne.com)
 *
 * Development of this audio library was funded by PJRC.COM, LLC by sales of
 * Teensy and Audio Adaptor boards. Please support PJRC's efforts to develop
 * open source software by purchasing other PJRC products.
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 */

#ifndef effect_bitcrusher_h_
#define effect_bitcrusher_h_

#include <Arduino.h>
#include "AudioStream.h"

class AudioEffectBitcrusher : public AudioStream {
  public:
    AudioEffectBitcrusher(void)
        : AudioStream(1, inputQueueArray), crushBits(16), sampleStep(1), samplePhase(0), heldSample(0) {}

    // Bit depth: 1..16 bits. 16 disables bit crushing.
    void bits(uint8_t b) {
        if (b > 16) {
            b = 16;
        } else if (b == 0) {
            b = 1;
        }

        crushBits = b;
    }

    // Desired effective sample rate in Hz.
    // The actual rate is quantized to an integer sample-step interval.
    void sampleRate(float hz);

    void update(void) override;

  private:
    uint8_t crushBits;
    uint8_t sampleStep;

    // Preserve sample-hold timing across AUDIO_BLOCK_SAMPLES boundaries.
    uint8_t samplePhase;
    int16_t heldSample;

    audio_block_t *inputQueueArray[1];
};

#endif // effect_bitcrusher_h_
