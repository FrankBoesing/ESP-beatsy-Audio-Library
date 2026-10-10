/*
 * Audio Library for Teensy 3.X
 * Copyright (c) 2015, Hedde Bosman
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "effect_midside.h"

#include <stdint.h>

#include "utility/dspinst.h"

OSPEED
void AudioEffectMidSide::update() {
    audio_block_t *blocka = receiveWritable(0);
    audio_block_t *blockb = receiveWritable(1);

    if (blocka == nullptr || blockb == nullptr) {
        if (blocka != nullptr) {
            release(blocka);
        }
        if (blockb != nullptr) {
            release(blockb);
        }
        return;
    }

    int16_t *a = blocka->data;
    int16_t *b = blockb->data;

    if (encoding) {
        // Use 32-bit intermediates so the sum and difference cannot overflow.
        // Arithmetic right shift matches the halving operations used by Teensy.
        for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            const int32_t left = a[i];
            const int32_t right = b[i];

            a[i] = (int16_t)((left + right) >> 1);
            b[i] = (int16_t)((left - right) >> 1);
        }
    } else {
        for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            const int32_t mid = a[i];
            const int32_t side = b[i];

            a[i] = saturate16(mid + side);
            b[i] = saturate16(mid - side);
        }
    }

    transmit(blocka, 0);
    transmit(blockb, 1);
    release(blocka);
    release(blockb);
}
