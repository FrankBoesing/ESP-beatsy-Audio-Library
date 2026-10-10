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

// Pink-noise generator based on:
// http://stenzel.waldorfmusic.de/post/pink/
// https://github.com/Stenzel/newshadeofpink
// New Shade of Pink (c) 2014 Stefan Stenzel
// Original terms: use for any purpose; if used in a commercial product,
// you should give the author one.

#include "synth_pinknoise.h"

#include <math.h>
#include <stdint.h>

#include "utility/dspinst.h"

uint32_t AudioSynthNoisePink::instance_cnt = 0;

// Let the compiler calculate the two 64-entry lookup tables for the 12-tap
// FIR filter. These coefficients preserve the original New Shade of Pink
// implementation.
#define PINK_FN(cf, m, shift) (2048 * cf * (2 * (((m) >> (shift)) & 1) - 1))
#define PINK_FA(n) \
    (int32_t)(PINK_FN(1.190566, n, 0) + PINK_FN(0.162580, n, 1) + \
              PINK_FN(0.002208, n, 2) + PINK_FN(0.025475, n, 3) + \
              PINK_FN(-0.001522, n, 4) + PINK_FN(0.007322, n, 5))
#define PINK_FB(n) \
    (int32_t)(PINK_FN(0.001774, n, 0) + PINK_FN(0.004529, n, 1) + \
              PINK_FN(-0.001561, n, 2) + PINK_FN(0.000776, n, 3) + \
              PINK_FN(-0.000486, n, 4) + PINK_FN(0.002017, n, 5))
#define PINK_FA8(n) PINK_FA(n), PINK_FA(n + 1), PINK_FA(n + 2), PINK_FA(n + 3), PINK_FA(n + 4), PINK_FA(n + 5), PINK_FA(n + 6), PINK_FA(n + 7)
#define PINK_FB8(n) PINK_FB(n), PINK_FB(n + 1), PINK_FB(n + 2), PINK_FB(n + 3), PINK_FB(n + 4), PINK_FB(n + 5), PINK_FB(n + 6), PINK_FB(n + 7)

const int32_t AudioSynthNoisePink::pfira[64] = {
    PINK_FA8(0), PINK_FA8(8), PINK_FA8(16), PINK_FA8(24),
    PINK_FA8(32), PINK_FA8(40), PINK_FA8(48), PINK_FA8(56)
};

const int32_t AudioSynthNoisePink::pfirb[64] = {
    PINK_FB8(0), PINK_FB8(8), PINK_FB8(16), PINK_FB8(24),
    PINK_FB8(32), PINK_FB8(40), PINK_FB8(48), PINK_FB8(56)
};

#define PINK_PM16(n) n, 0x80, 0x40, 0x80, 0x20, 0x80, 0x40, 0x80, 0x10, 0x80, 0x40, 0x80, 0x20, 0x80, 0x40, 0x80
const uint8_t AudioSynthNoisePink::pnmask[256] = {
    PINK_PM16(0), PINK_PM16(8), PINK_PM16(4), PINK_PM16(8),
    PINK_PM16(2), PINK_PM16(8), PINK_PM16(4), PINK_PM16(8),
    PINK_PM16(1), PINK_PM16(8), PINK_PM16(4), PINK_PM16(8),
    PINK_PM16(2), PINK_PM16(8), PINK_PM16(4), PINK_PM16(8)
};

AudioSynthNoisePink::AudioSynthNoisePink()
    : AudioStream(0, nullptr),
      plfsr(0x5EED41F5U + instance_cnt++),
      pinc(0x0CCC),
      pdec(0x0CCC),
      paccu(0),
      pncnt(0),
      level(0) {}

void AudioSynthNoisePink::amplitude(float n) {
    if (!isfinite(n)) {
        n = 0.0f;
    } else if (n < 0.0f) {
        n = 0.0f;
    } else if (n > 1.0f) {
        n = 1.0f;
    }

    const int32_t newLevel = (int32_t)(n * 65536.0f);

    portENTER_CRITICAL(&levelMux);
    level = newLevel;
    portEXIT_CRITICAL(&levelMux);
}

// One pink-noise update step. The signed-multiply helper is shared with the
// other fixed-point DSP components in utility/dspinst.h.
#define PINK_STEP(bitmask, out)                                      \
    do {                                                             \
        bit = (int32_t)(lfsr >> 31);                                 \
        dec &= ~(bitmask);                                           \
        lfsr <<= 1;                                                  \
        dec |= inc & (bitmask);                                      \
        inc ^= bit & taps;                                           \
        accu += inc - dec;                                           \
        lfsr ^= (uint32_t)(bit & taps);                              \
        (out) = accu + pfira[lfsr & 0x3FU] + pfirb[(lfsr >> 6) & 0x3FU]; \
    } while (0)

OSPEED
void AudioSynthNoisePink::update() {
    int32_t gain;

    portENTER_CRITICAL(&levelMux);
    gain = level;
    portEXIT_CRITICAL(&levelMux);

    if (gain == 0) {
        return;
    }

    audio_block_t *block = allocate();
    if (block == nullptr) {
        return;
    }

    int32_t inc = pinc;
    int32_t dec = pdec;
    int32_t accu = paccu;
    uint32_t lfsr = plfsr;
    int32_t bit;
    const int32_t taps = 0x46000001;

    // Same eight-tap mask cadence as the original implementation, but write
    // individual int16 samples rather than aliasing the audio buffer as uint32_t.
    static const uint16_t pairMasks[8] = {
        0, 0x0400, 0x0200, 0x0400, 0x0100, 0x0400, 0x0200, 0x0400
    };

    uint32_t sampleIndex = 0;
    uint32_t pairIndex = 0;
    while (sampleIndex < AUDIO_BLOCK_SAMPLES) {
        const uint32_t pairInGroup = pairIndex & 7U;
        const int32_t mask = pairInGroup == 0
                                 ? pnmask[pncnt++]
                                 : pairMasks[pairInGroup];

        int32_t n1;
        PINK_STEP(mask, n1);
        n1 = signed_multiply_32x16b(gain, (uint32_t)n1);
        block->data[sampleIndex++] = (int16_t)n1;

        if (sampleIndex < AUDIO_BLOCK_SAMPLES) {
            int32_t n2;
            PINK_STEP(0x0800, n2);
            n2 = signed_multiply_32x16b(gain, (uint32_t)n2);
            block->data[sampleIndex++] = (int16_t)n2;
        }

        ++pairIndex;
    }

    pinc = inc;
    pdec = dec;
    paccu = accu;
    plfsr = lfsr;

    transmit(block);
    release(block);
}

#undef PINK_STEP
#undef PINK_PM16
#undef PINK_FA8
#undef PINK_FB8
#undef PINK_FA
#undef PINK_FB
#undef PINK_FN
