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

#include "synth_tonesweep.h"

#include <math.h>
#include <stdint.h>

#include "utility/dspinst.h"

bool AudioSynthToneSweep::play(float t_amp, int t_lo, int t_hi, float t_time) {
    const uint32_t sampleRate = (uint32_t)AUDIO_SAMPLE_RATE_EXACT;

    if (!isfinite(t_amp) || !isfinite(t_time) ||
        t_amp < 0.0f || t_amp > 1.0f ||
        t_lo < 1 || t_hi < 1 ||
        t_lo >= (int)(sampleRate / 2U) ||
        t_hi >= (int)(sampleRate / 2U) ||
        t_time <= 0.0f || sampleRate == 0U) {
        return false;
    }

    // Store the sweep duration in samples. Very long finite times are accepted
    // and saturated at UINT64_MAX rather than overflowing during conversion.
    const double requestedSamples = (double)t_time * (double)sampleRate;
    uint64_t totalSamples;
    if (requestedSamples >= 18446744073709551616.0) {
        totalSamples = UINT64_MAX;
    } else if (requestedSamples < 1.0) {
        totalSamples = 1;
    } else {
        totalSamples = (uint64_t)(requestedSamples + 0.5);
        if (totalSamples == 0) {
            totalSamples = 1;
        }
    }

    const uint64_t startFrequency = (uint64_t)(uint32_t)t_lo << 32;
    const uint64_t targetFrequency = (uint64_t)(uint32_t)t_hi << 32;
    const uint32_t frequencyDistance =
        (t_hi >= t_lo) ? (uint32_t)(t_hi - t_lo) : (uint32_t)(t_lo - t_hi);
    const uint64_t deltaQ32 = (uint64_t)frequencyDistance << 32;

    const uint64_t increment = deltaQ32 / totalSamples;
    const uint64_t remainder = deltaQ32 % totalSamples;
    const int16_t amplitude = (int16_t)(t_amp * 32767.0f);
    const int direction = (t_hi >= t_lo) ? 1 : -1;

    portENTER_CRITICAL(&stateMux);
    tone_amp = amplitude;
    tone_freq = startFrequency;
    tone_target = targetFrequency;
    tone_phase = 0;
    tone_incr = increment;
    tone_remainder = remainder;
    remainder_accum = 0;
    sweep_samples = totalSamples;
    samples_elapsed = 0;
    tone_sign = direction;
    sweep_busy = 1;
    ++stateGeneration;
    portEXIT_CRITICAL(&stateMux);

    return true;
}

unsigned char AudioSynthToneSweep::isPlaying() {
    portENTER_CRITICAL(&stateMux);
    const unsigned char busy = sweep_busy;
    portEXIT_CRITICAL(&stateMux);
    return busy;
}

OSPEED
void AudioSynthToneSweep::update() {
    int16_t amplitude;
    uint64_t frequency;
    uint64_t targetFrequency;
    uint64_t frequencyIncrement;
    uint64_t frequencyRemainder;
    uint64_t remainderAccumulator;
    uint64_t totalSamples;
    uint64_t samplesElapsed;
    uint32_t phase;
    int direction;
    uint32_t generation;
    unsigned char busy;

    portENTER_CRITICAL(&stateMux);
    busy = sweep_busy;
    amplitude = tone_amp;
    frequency = tone_freq;
    targetFrequency = tone_target;
    frequencyIncrement = tone_incr;
    frequencyRemainder = tone_remainder;
    remainderAccumulator = remainder_accum;
    totalSamples = sweep_samples;
    samplesElapsed = samples_elapsed;
    phase = tone_phase;
    direction = tone_sign;
    generation = stateGeneration;
    portEXIT_CRITICAL(&stateMux);

    if (!busy || totalSamples == 0) {
        return;
    }

    audio_block_t *block = allocate();
    if (block == nullptr) {
        return;
    }

    const uint32_t sampleRate = (uint32_t)AUDIO_SAMPLE_RATE_EXACT;
    uint32_t i = 0;
    bool stillPlaying = true;

    for (; i < AUDIO_BLOCK_SAMPLES; ++i) {
        if (!stillPlaying || samplesElapsed >= totalSamples) {
            break;
        }

        // sin_q15_phase_q16() returns Q15.16 from a full-turn Q0.32 phase.
        const int32_t sineQ15 = sin_q15_phase_q16(phase) >> 16;
        block->data[i] = (int16_t)((sineQ15 * (int32_t)amplitude) >> 15);

        // Q32.32 Hz divided by sample rate gives a Q0.32 phase increment.
        phase += (uint32_t)(frequency / sampleRate);

        // Divide the frequency delta into quotient and remainder so the sweep
        // reaches its target even when the requested duration is very long.
        uint64_t delta = frequencyIncrement;
        if (frequencyRemainder != 0) {
            const uint64_t threshold = totalSamples - frequencyRemainder;
            if (remainderAccumulator >= threshold) {
                remainderAccumulator -= threshold;
                ++delta;
            } else {
                remainderAccumulator += frequencyRemainder;
            }
        }

        if (direction > 0) {
            const uint64_t remaining = targetFrequency - frequency;
            frequency = (delta >= remaining) ? targetFrequency : frequency + delta;
        } else {
            const uint64_t remaining = frequency - targetFrequency;
            frequency = (delta >= remaining) ? targetFrequency : frequency - delta;
        }

        ++samplesElapsed;
        if (samplesElapsed >= totalSamples) {
            frequency = targetFrequency;
            stillPlaying = false;
            ++i;
            break;
        }
    }

    while (i < AUDIO_BLOCK_SAMPLES) {
        block->data[i++] = 0;
    }

    // Update shared state only if play() has not started a newer sweep while
    // this block was being rendered.
    portENTER_CRITICAL(&stateMux);
    if (stateGeneration == generation) {
        tone_freq = frequency;
        tone_phase = phase;
        remainder_accum = remainderAccumulator;
        samples_elapsed = samplesElapsed;
        sweep_busy = stillPlaying ? 1 : 0;
        if (!stillPlaying) {
            tone_incr = 0;
            tone_remainder = 0;
            remainder_accum = 0;
        }
        ++stateGeneration;
    }
    portEXIT_CRITICAL(&stateMux);

    transmit(block, 0);
    release(block);
}
