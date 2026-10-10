/*
 * Audio Library for Teensy 3.X
 * Copyright (c) 2016, Byron Jacquot, SparkFun Electronics
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

#include "synth_simple_drum.h"

#include <math.h>
#include <stdint.h>

#include "synth_waveform.h"
#include "utility/dspinst.h"

AudioSynthSimpleDrum::AudioSynthSimpleDrum() : AudioStream(1, inputQueueArray) {
    length(600);
    frequency(60.0f);

    // Keep the original default argument. pitchMod() clamps it to 1.0f,
    // which gives the characteristic strong initial pitch drop.
    pitchMod(0x200);
    secondMix(0.0f);
}

void AudioSynthSimpleDrum::noteOn() {
    portENTER_CRITICAL(&stateMux);
    wav_phasor = 0;
    wav_phasor2 = 0;
    env_lin_current = 0x7fff0000;
    ++noteGeneration;
    portEXIT_CRITICAL(&stateMux);
}

void AudioSynthSimpleDrum::frequency(float freq) {
    if (!isfinite(freq)) {
        return;
    }
    if (freq < 0.0f) {
        freq = 0.0f;
    } else if (freq > AUDIO_SAMPLE_RATE_EXACT / 2.0f) {
        freq = AUDIO_SAMPLE_RATE_EXACT / 2.0f;
    }

    const uint32_t increment =  (uint32_t)(freq * (2147483647.0f / AUDIO_SAMPLE_RATE_EXACT) + 0.5f);

    portENTER_CRITICAL(&configMux);
    wav_increment = increment;
    portEXIT_CRITICAL(&configMux);
}

void AudioSynthSimpleDrum::length(int32_t milliseconds) {
    if (milliseconds < 0) {
        return;
    }
    if (milliseconds > 5000) {
        milliseconds = 5000;
    }

    // Avoid the divide-by-zero case in the upstream implementation.
    uint32_t lengthSamples;
    if (milliseconds == 0) {
        lengthSamples = 1;
    } else {
        lengthSamples = (uint32_t)((float)milliseconds * (AUDIO_SAMPLE_RATE_EXACT / 1000.0f));
        if (lengthSamples == 0) {
            lengthSamples = 1;
        }
    }

    const int32_t decrement = (int32_t)(0x7fff0000U / lengthSamples);

    portENTER_CRITICAL(&configMux);
    env_decrement = decrement;
    portEXIT_CRITICAL(&configMux);
}

void AudioSynthSimpleDrum::secondMix(float level) {
    if (!isfinite(level)) {
        level = 0.0f;
    } else if (level < 0.0f) {
        level = 0.0f;
    } else if (level > 1.0f) {
        level = 1.0f;
    }

    // At level 1, blend the second oscillator at half-scale maximum.
    const int16_t amplitude2 = (int16_t)(level * 0x3fff);
    const int16_t amplitude1 = (int16_t)(0x7fff - amplitude2);

    portENTER_CRITICAL(&configMux);
    wav_amplitude1 = amplitude1;
    wav_amplitude2 = amplitude2;
    portEXIT_CRITICAL(&configMux);
}

void AudioSynthSimpleDrum::pitchMod(float depth) {
    if (!isfinite(depth)) {
        depth = 0.0f;
    } else if (depth < 0.0f) {
        depth = 0.0f;
    } else if (depth > 1.0f) {
        depth = 1.0f;
    }

    const int32_t intDepth = (int32_t)(depth * 0x7fff);
    int32_t calc;

    // Map 0..1 to the original 2.14 pitch-modulation range.
    // Zero is centred at depth=0.5; lower values pitch downward,
    // higher values create a stronger upward transient at note onset.
    if (intDepth < 0x4000) {
        calc = ((0x4000 - intDepth) * 0x3000) >> 14;
        calc = -calc;
    } else {
        calc = ((intDepth - 0x4000) * 0xc000) >> 14;
    }

    portENTER_CRITICAL(&configMux);
    wav_pitch_mod = calc;
    portEXIT_CRITICAL(&configMux);
}

OSPEED
void AudioSynthSimpleDrum::update() {
    uint32_t increment;
    int32_t envelopeDecrement;
    int16_t amplitude1;
    int16_t amplitude2;
    int32_t pitchModulation;

    portENTER_CRITICAL(&configMux);
    increment = wav_increment;
    envelopeDecrement = env_decrement;
    amplitude1 = wav_amplitude1;
    amplitude2 = wav_amplitude2;
    pitchModulation = wav_pitch_mod;
    portEXIT_CRITICAL(&configMux);

    uint32_t phase1;
    uint32_t phase2;
    int32_t envelope;
    uint32_t generation;

    portENTER_CRITICAL(&stateMux);
    phase1 = wav_phasor;
    phase2 = wav_phasor2;
    envelope = env_lin_current;
    generation = noteGeneration;
    portEXIT_CRITICAL(&stateMux);

    // No note is active, so there is no need to allocate a silent block.
    if (envelope < 0x0000ffff) {
        return;
    }

    audio_block_t *block = allocate();
    if (block == nullptr) {
        return;
    }

    const bool doSecond = amplitude2 > 50;

    for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        if (envelope < 0x0000ffff) {
            block->data[i] = 0;
            continue;
        }

        // The original envelope is Q15.16. Squaring its high word produces
        // the Q2.30 curve used for the quasi-exponential amplitude envelope.
        envelope -= envelopeDecrement;
        const int32_t envelopeQ15 = envelope >> 16;
        const int32_t envelopeSquared = envelopeQ15 * envelopeQ15;

        phase1 += increment;

        // Pitch modulation is strongest at the start and fades with the
        // square of the envelope. Keep the original fixed-point scaling.
        const int32_t mod = signed_multiply_32x16b(envelopeSquared, (uint32_t)(pitchModulation >> 1)) >> 13;
        const int32_t mod2 = signed_multiply_32x16b((int32_t)(increment << 3), (uint32_t)(mod >> 1));

        phase1 = (phase1 + (uint32_t)mod2) & 0x7fffffffU;

        if (doSecond) {
            // A perfect fifth: 1.5 times the fundamental increment.
            phase2 += increment;
            phase2 += increment >> 1;
            phase2 += (uint32_t)mod2;
            phase2 += (uint32_t)(mod2 >> 1);
            phase2 &= 0x7fffffffU;
        }

        uint32_t index = phase1 >> 23;
        int32_t sineLeft = AudioWaveformSine[index];
        int32_t sineRight = AudioWaveformSine[index + 1U];
        int32_t delta = sineRight - sineLeft;
        uint32_t scale = (phase1 >> 7) & 0xffffU;
        delta = (delta * (int32_t)scale) >> 16;
        int32_t interpolated = sineLeft + delta;

        if (doSecond) {
            index = phase2 >> 23;
            sineLeft = AudioWaveformSine[index];
            sineRight = AudioWaveformSine[index + 1U];
            delta = sineRight - sineLeft;
            scale = (phase2 >> 7) & 0xffffU;
            delta = (delta * (int32_t)scale) >> 16;
            const int32_t interpolated2 = sineLeft + delta;

            const int32_t second = (interpolated2 * amplitude2) >> 15;
            interpolated = ((interpolated * amplitude1) >> 15) + second;
        }

        const int32_t output = signed_multiply_32x16b(envelopeSquared, (uint32_t)interpolated) >> 15;
        block->data[i] = saturate16(output);
    }

    portENTER_CRITICAL(&stateMux);
    if (noteGeneration == generation) {
        wav_phasor = phase1;
        wav_phasor2 = phase2;
        env_lin_current = envelope;
    }
    portEXIT_CRITICAL(&stateMux);

    transmit(block, 0);
    release(block);
}
