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

#include "synth_karplusstrong.h"

#include <math.h>
#include <stdint.h>

#include "utility/dspinst.h"

namespace {

// Park-Miller PRNG modulo 2^31-1. This is the portable equivalent of the
// split multiply used in the original Teensy implementation.
static inline uint32_t karplusStrongNextSeed(uint32_t seed) {
    const uint32_t high = 16807U * (seed >> 16);
    uint32_t low = 16807U * (seed & 0xFFFFU);

    low += (high & 0x7FFFU) << 16;
    low += high >> 15;
    low = (low & 0x7FFFFFFFU) + (low >> 31);

    return low == 2147483647U ? 1U : low;
}

} // namespace

uint32_t AudioSynthKarplusStrong::seed = 1U;

AudioSynthKarplusStrong::AudioSynthKarplusStrong() : AudioStream(0, nullptr) {}

void AudioSynthKarplusStrong::noteOn(float frequency, float velocity) {
    if (!isfinite(frequency) || frequency <= 0.0f || !isfinite(velocity) || velocity <= 0.0f) {
        noteOff(velocity);
        return;
    }

    // Treat velocity as normalized amplitude and clamp out-of-range input.
    if (velocity > 1.0f) {
        velocity = 1.0f;
    }

    float requestedLength = AUDIO_SAMPLE_RATE_EXACT / frequency + 0.5f;
    uint32_t length = requestedLength >= (float)MAX_BUFFER_LENGTH ? MAX_BUFFER_LENGTH : (uint32_t)requestedLength;

    // A positive frequency can still produce a zero-length delay at high
    // frequencies. Keep at least one delay sample to prevent invalid indexing.
    if (length < 1U) {
        length = 1U;
    }

    const int32_t newMagnitude = (int32_t)(velocity * 65535.0f);

    portENTER_CRITICAL(&stateMux);
    magnitude = newMagnitude;
    bufferLen = (uint16_t)length;
    bufferIndex = 0;
    state = 1;
    ++stateGeneration;
    portEXIT_CRITICAL(&stateMux);
}

void AudioSynthKarplusStrong::noteOff(float /*velocity*/) {
    portENTER_CRITICAL(&stateMux);
    state = 0;
    ++stateGeneration;
    portEXIT_CRITICAL(&stateMux);
}

OSPEED
void AudioSynthKarplusStrong::update() {
    uint16_t localBufferLen;
    uint16_t localBufferIndex;
    int32_t localMagnitude;
    uint32_t localGeneration;
    uint8_t localState;

    portENTER_CRITICAL(&stateMux);
    localState = state;
    if (localState == 0 || bufferLen == 0) {
        portEXIT_CRITICAL(&stateMux);
        return;
    }

    localBufferLen = bufferLen;
    localBufferIndex = bufferIndex;
    localMagnitude = magnitude;
    localGeneration = stateGeneration;
    portEXIT_CRITICAL(&stateMux);

    if (localState == 1) {
        // Generate the excitation outside the critical section. If noteOn()
        // arrives during this loop, the generation check below rejects this
        // initialization and the new note is prepared by the next update.
        uint32_t localSeed = seed;
        for (uint16_t i = 0; i < localBufferLen; ++i) {
            localSeed = karplusStrongNextSeed(localSeed);
            buffer[i] = (int16_t)signed_multiply_32x16b(localMagnitude, localSeed);
        }

        portENTER_CRITICAL(&stateMux);
        const bool initialized = stateGeneration == localGeneration && state == 1;
        if (initialized) {
            seed = localSeed;
            state = 2;
            localBufferIndex = bufferIndex;
        }
        portEXIT_CRITICAL(&stateMux);

        if (!initialized) {
            return;
        }
        localState = 2;
    }

    if (localState != 2 || localBufferLen == 0) {
        return;
    }

    audio_block_t *block = allocate();
    if (block == nullptr) {
        portENTER_CRITICAL(&stateMux);
        if (stateGeneration == localGeneration && state == 2) {
            state = 0;
            ++stateGeneration;
        }
        portEXIT_CRITICAL(&stateMux);
        return;
    }

    int16_t prior = localBufferIndex > 0 ? buffer[localBufferIndex - 1U] : buffer[localBufferLen - 1U];

#pragma GCC unroll 4
    for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        const int16_t input = buffer[localBufferIndex];

        // Averaging adjacent samples with a slight loss creates the
        // characteristic decay of the Karplus-Strong string model.
        const int32_t output = ((int32_t)input * 32686 + (int32_t)prior * 32686) >> 16;

        block->data[i] = saturate16(output);
        buffer[localBufferIndex] = block->data[i];
        prior = input;

        if (++localBufferIndex >= localBufferLen) {
            localBufferIndex = 0;
        }
    }

    bool stillCurrent = false;
    portENTER_CRITICAL(&stateMux);
    if (stateGeneration == localGeneration && state == 2) {
        bufferIndex = localBufferIndex;
        stillCurrent = true;
    }
    portEXIT_CRITICAL(&stateMux);

    if (stillCurrent) {
        transmit(block);
    }
    release(block);
}
