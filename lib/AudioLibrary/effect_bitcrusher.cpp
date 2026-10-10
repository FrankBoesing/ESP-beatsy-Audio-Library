
/*
 * Audio Library for Teensy 3.X
 * Copyright (c) 2014, Jonathan Payne (jon@jonnypayne.com)
 *
 * Derived from the Teensy Audio Library effect_bitcrusher.cpp.
 * Original sample-rate reduction technique based on Pete Brown's bitcrusher.
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 */

#include "effect_bitcrusher.h"

#include <cmath>
#include <stdint.h>

namespace {

// Convert a 16-bit sample to the requested bit depth by clearing its
// least-significant bits. The signed reconstruction avoids converting an
// out-of-range uint16_t value directly to int16_t.
static inline int16_t quantizeSample(int16_t sample, uint16_t mask) {
    const uint16_t quantized = static_cast<uint16_t>(sample) & mask;

    const int32_t signedValue =
        (quantized & 0x8000u) ? static_cast<int32_t>(quantized) - 65536 : static_cast<int32_t>(quantized);

    return static_cast<int16_t>(signedValue);
}

} // namespace

void AudioEffectBitcrusher::sampleRate(float hz) {
    // Invalid requests are ignored; do not divide by zero or convert
    // NaN/Inf to an integer.
    if (!(hz > 0.0f) || !std::isfinite(hz)) {
        return;
    }

    const float inputRate = AudioStream::sampleRate();

    if (!(inputRate > 0.0f) || !std::isfinite(inputRate)) {
        return;
    }

    const float ratio = inputRate / hz;
    int step;

    if (!std::isfinite(ratio) || ratio >= 64.0f) {
        step = 64;
    } else if (ratio <= 1.0f) {
        step = 1;
    } else {
        step = static_cast<int>(ratio + 0.5f);
    }

    if (step < 1) {
        step = 1;
    } else if (step > 64) {
        step = 64;
    }

    if (sampleStep != step) {
        sampleStep = static_cast<uint8_t>(step);

        // Start a new sample-hold interval using the next input sample.
        samplePhase = 0;
    }
}

void AudioEffectBitcrusher::update(void) {
    audio_block_t *block;

    const uint8_t bits = crushBits;
    const uint8_t step = sampleStep;

    // Fast path: no processing required.
    // Share the read-only block instead of allocating a writable copy.
    if (bits == 16 && step <= 1) {
        block = receiveReadOnly();
        if (block == nullptr) {
            return;
        }

        transmit(block);
        release(block);
        return;
    }

    block = receiveWritable();
    if (block == nullptr) {
        return;
    }

    // bits is guaranteed to be in the range 1..16.
    // A full-width mask means no bit-depth reduction.
    const uint16_t bitMask = (bits >= 16) ? 0xFFFFu : static_cast<uint16_t>(0xFFFFu << (16u - bits));

    if (step <= 1) {
        // Bit-depth reduction only.
        for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            block->data[i] = quantizeSample(block->data[i], bitMask);
        }
    } else {
        // Sample-rate reduction: hold each selected sample for 'step'
        // output samples. Preserve the phase between audio blocks so the
        // reduction interval does not restart at every block boundary.
        uint8_t phase = samplePhase;
        int16_t held = heldSample;

        for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            if (phase == 0) {
                held = quantizeSample(block->data[i], bitMask);
            }

            block->data[i] = held;

            ++phase;
            if (phase >= step) {
                phase = 0;
            }
        }

        samplePhase = phase;
        heldSample = held;
    }

    transmit(block);
    release(block);
}
