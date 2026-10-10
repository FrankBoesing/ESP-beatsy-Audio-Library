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

#include "effect_chorus.h"

#include <string.h>

#include <limits.h>

// The accumulator is int32_t. This cap keeps the sum of int16 samples in range.
static const int32_t CHORUS_MAX_VOICES = 65535;

int32_t AudioEffectChorus::limitVoices(int32_t nChorus, int32_t delayLength) {
    if (nChorus <= 1 || delayLength <= 0) {
        return nChorus;
    }

    int32_t maxVoices = delayLength + 1;
    if (maxVoices > CHORUS_MAX_VOICES) {
        maxVoices = CHORUS_MAX_VOICES;
    }

    return nChorus > maxVoices ? maxVoices : nChorus;
}

bool AudioEffectChorus::begin(short *delayline, int delayLength, int nChorus) {
    l_delayline = nullptr;
    delay_length = 0;
    l_circ_idx = 0;
    num_chorus = 1;

    if (delayline == nullptr || delayLength < 10 || nChorus < 1) {
        return false;
    }

    l_delayline = delayline;
    delay_length = delayLength / 2;
    num_chorus = limitVoices(nChorus, delay_length);

    // Start with a deterministic, silent history instead of reading whatever
    // happened to be in a caller-owned, potentially uninitialized buffer.
    memset(l_delayline, 0, (size_t)delay_length * sizeof(short));
    return true;
}

void AudioEffectChorus::voices(int nChorus) {
    num_chorus = limitVoices(nChorus, delay_length);
}

OSPEED
void AudioEffectChorus::update() {
    audio_block_t *block = receiveWritable(0);
    if (block == nullptr) {
        return;
    }

    // If begin() has not succeeded, behave as a transparent effect.
    if (l_delayline == nullptr || delay_length <= 0) {
        transmit(block);
        release(block);
        return;
    }

    const int32_t chorusVoices = num_chorus;

    // Bypass still records the input so enabling chorus does not start with
    // an entirely stale delay line.
    if (chorusVoices <= 1) {
        for (int32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            ++l_circ_idx;
            if (l_circ_idx >= delay_length) {
                l_circ_idx = 0;
            }
            l_delayline[l_circ_idx] = block->data[i];
        }

        transmit(block);
        release(block);
        return;
    }

    // Spread voices around the circular delay line. Keep the Teensy spacing
    // formula to preserve its sound and delay-line sizing behaviour.
    const int32_t offset = delay_length / (chorusVoices - 1) - 1;

    for (int32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        ++l_circ_idx;
        if (l_circ_idx >= delay_length) {
            l_circ_idx = 0;
        }

        l_delayline[l_circ_idx] = block->data[i];

        int32_t sum = 0;
        int32_t c_idx = l_circ_idx;

        for (int32_t voice = 0; voice < chorusVoices; ++voice) {
            sum += l_delayline[c_idx];
            c_idx -= offset;
            if (c_idx < 0) {
                c_idx += delay_length;
            }
        }

        // The mean of int16 samples remains in the int16 range.
        block->data[i] = (short)(sum / chorusVoices);
    }

    transmit(block);
    release(block);
}
