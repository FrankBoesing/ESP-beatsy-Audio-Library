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

#ifndef effect_chorus_h_
#define effect_chorus_h_

#include "AudioStream.h"

// Kept for compatibility with the Teensy Audio Library API.
#define CHORUS_DELAY_PASSTHRU -1

class AudioEffectChorus : public AudioStream {
  public:
    AudioEffectChorus() : AudioStream(1, inputQueueArray) {}

    // The caller owns delayline and must keep it alive while this effect uses it.
    // As in the Teensy implementation, only delayLength / 2 samples are used.
    bool begin(short *delayline, int delayLength, int nChorus);
    void update() override;

    // Number of voices, including the original signal. Values <= 1 bypass
    // the effect while continuing to feed the delay line. voices(0) is OFF.
    void voices(int nChorus);

  private:
    static int32_t limitVoices(int32_t nChorus, int32_t delayLength);

    audio_block_t *inputQueueArray[1] = {};
    short *l_delayline = nullptr;
    int32_t l_circ_idx = 0;
    int32_t num_chorus = 2;
    int32_t delay_length = 0;
};

#endif
