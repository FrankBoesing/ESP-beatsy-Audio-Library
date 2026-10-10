/*
 * Waveshaper for Teensy 3.X audio
 *
 * Copyright (c) 2017 Damien Clarke, http://damienclarke.me
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "effect_waveshaper.h"

#include <cmath>
#include <new>

AudioEffectWaveshaper::~AudioEffectWaveshaper() {
    delete[] waveshape;
}

void AudioEffectWaveshaper::shape(float *curve, int length) {
    // The table must contain 2^N + 1 points. Keep the current curve unchanged
    // if the new configuration is invalid or allocation fails.
    if (curve == nullptr || length < 2 || length > 32769 ||
        ((length - 1) & (length - 2)) != 0) {
        return;
    }

    int16_t *newShape = new (std::nothrow) int16_t[length];
    if (newShape == nullptr) {
        return;
    }

    // Convert normalized float samples to signed 16-bit audio range. Clamp
    // invalid/out-of-range input so conversion never overflows or sees NaN/Inf.
    for (int i = 0; i < length; ++i) {
        float value = curve[i];

        if (!std::isfinite(value)) {
            value = 0.0f;
        } else if (value > 1.0f) {
            value = 1.0f;
        } else if (value < -1.0f) {
            value = -1.0f;
        }

        newShape[i] = (int16_t)(32767.0f * value);
    }

    // lerpshift maps the full uint16_t input range onto the table segments.
    int index = length - 1;
    int16_t newLerpShift = 16;
    while (index >>= 1) {
        --newLerpShift;
    }

    int16_t *oldShape = waveshape;
    waveshape = newShape;
    lerpshift = newLerpShift;
    delete[] oldShape;
}

OSPEED
void AudioEffectWaveshaper::update() {
    // With no transfer curve configured, pass the signal through unchanged.
    if (waveshape == nullptr) {
        audio_block_t *block = receiveReadOnly();
        if (block == nullptr) {
            return;
        }

        transmit(block);
        release(block);
        return;
    }

    audio_block_t *block = receiveWritable();
    if (block == nullptr) {
        return;
    }

    const int16_t *table = waveshape;
    const int16_t shift = lerpshift;

    // A two-point table uses a 16-bit interpolation fraction. Use a 64-bit
    // intermediate here: the full signed endpoint difference times 65535
    // can exceed INT32_MAX. Longer tables have shift <= 15 and fit int32_t.
    if (shift == 16) {
        const int32_t ya = table[0];
        const int32_t yb = table[1];
        const int32_t delta = yb - ya;

        for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            const uint32_t x = (uint16_t)((int32_t)block->data[i] + 32768);
            const int32_t adjustment = (int32_t)(((int64_t)delta * x) >> 16);
            block->data[i] = (int16_t)(ya + adjustment);
        }
    } else {
        for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            const uint32_t x = (uint16_t)((int32_t)block->data[i] + 32768);
            const uint32_t xa = x >> shift;
            const int32_t ya = table[xa];
            const int32_t yb = table[xa + 1];
            const uint32_t fraction = x - (xa << shift);
            const int32_t adjustment = ((yb - ya) * (int32_t)fraction) >> shift;
            block->data[i] = (int16_t)(ya + adjustment);
        }
    }

    transmit(block);
    release(block);
}
