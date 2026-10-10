/*
 * Wavefolder effect for Teensy Audio library
 *
 * Copyright (c) 2020, Mark Tillotson
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

#include "effect_wavefolder.h"

OSPEED
void AudioEffectWaveFolder::update() {
    audio_block_t *blocka = receiveWritable(0);
    audio_block_t *blockb = receiveReadOnly(1);

    if (blocka == nullptr || blockb == nullptr) {
        if (blocka != nullptr) {
            release(blocka);
        }
        if (blockb != nullptr) {
            release(blockb);
        }
        return;
    }

    int16_t *pa = blocka->data;
    const int16_t *pb = blockb->data;

    for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        const int32_t a = pa[i];
        const int32_t b = pb[i];

        // Scale to allow folding up to approximately 16 times per polarity.
        // The 32-bit product is sufficient for the full int16_t input range.
        int32_t folded = (a * b + 0x400) >> 11;

        // Detect which fold band the sample occupies.
        const int32_t flip = ((folded + 0x8000) >> 16) & 1;

        // Reflect alternate bands and wrap to the 16-bit output bit pattern.
        const uint16_t result = (uint16_t)(0xFFFF & (flip ? ~folded : folded));
        pa[i] = (int16_t)result;
    }

    transmit(blocka);
    release(blocka);
    release(blockb);
}
