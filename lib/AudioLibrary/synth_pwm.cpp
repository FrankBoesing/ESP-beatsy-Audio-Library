/*
 * Audio Library for Teensy 3.X
 * Copyright (c) 2017, Paul Stoffregen, paul@pjrc.com
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

#include "synth_pwm.h"

#include <math.h>
#include <stdint.h>

AudioSynthWaveformPWM::AudioSynthWaveformPWM() : AudioStream(1, inputQueueArray) {
    frequency(440.0f);
    amplitude(0.0f);
}

void AudioSynthWaveformPWM::frequency(float freq) {
    if (!isfinite(freq)) {
        return;
    }
    if (freq < 1.0f) {
        freq = 1.0f;
    } else if (freq > AUDIO_SAMPLE_RATE_EXACT / 4.0f) {
        freq = AUDIO_SAMPLE_RATE_EXACT / 4.0f;
    }

    // Store the duration of a half-cycle in Q16.16 sample units.
    const uint32_t newDuration = (uint32_t)((AUDIO_SAMPLE_RATE_EXACT * 65536.0f + freq) / (freq * 2.0f));

    portENTER_CRITICAL(&configMux);
    duration = newDuration;
    portEXIT_CRITICAL(&configMux);
}

void AudioSynthWaveformPWM::amplitude(float n) {
    if (!isfinite(n)) {
        return;
    }
    if (n < 0.0f) {
        n = 0.0f;
    } else if (n > 1.0f) {
        n = 1.0f;
    }

    const int32_t newMagnitude = (int32_t)(n * 32767.0f);

    portENTER_CRITICAL(&configMux);
    requestedMagnitude = newMagnitude;
    ++amplitudeGeneration;
    portEXIT_CRITICAL(&configMux);
}

OSPEED
void AudioSynthWaveformPWM::update() {
    audio_block_t *modulationBlock = receiveReadOnly();

    uint32_t localDuration;
    int32_t localRequestedMagnitude;
    uint32_t localAmplitudeGeneration;

    portENTER_CRITICAL(&configMux);
    localDuration = duration;
    localRequestedMagnitude = requestedMagnitude;
    localAmplitudeGeneration = amplitudeGeneration;
    portEXIT_CRITICAL(&configMux);

    // Like the Teensy implementation, changing amplitude restarts the pulse
    // polarity at positive, but does not reset the current phase accumulator.
    if (localAmplitudeGeneration != appliedAmplitudeGeneration) {
        magnitude = localRequestedMagnitude;
        appliedAmplitudeGeneration = localAmplitudeGeneration;
    }

    if (magnitude == 0) {
        if (modulationBlock != nullptr) {
            release(modulationBlock);
        }
        return;
    }

    audio_block_t *block = allocate();
    if (block == nullptr) {
        if (modulationBlock != nullptr) {
            release(modulationBlock);
        }
        return;
    }

    if (modulationBlock != nullptr) {
        for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            elapsed += 65536U;

            int32_t control = modulationBlock->data[i];
            if (magnitude < 0) {
                control = -control;
            }

            // Map the signed modulation sample onto a 0..100% pulse width.
            // Q16 duration can approach 2^32 / 2 at low output frequencies,
            // so use a 64-bit intermediate for this product.
            const uint32_t pulseDuration = (uint32_t)(((uint64_t)(control + 32768) * (uint64_t)localDuration) >> 15);

            int32_t output;
            if (elapsed < pulseDuration) {
                output = magnitude;
            } else {
                uint32_t overshoot = elapsed - pulseDuration;

                // Keep the edge interpolation fraction in Q16.16's low 16
                // bits, as intended by the original implementation.
                if (overshoot > 65535U) {
                    overshoot = 65535U;
                }
                elapsed = overshoot;

                output = magnitude - ((magnitude * (int32_t)elapsed) >> 15);
                magnitude = -magnitude;
            }

            block->data[i] = (int16_t)output;
        }

        release(modulationBlock);
    } else {
        for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            elapsed += 65536U;

            int32_t output;
            if (elapsed < localDuration) {
                output = magnitude;
            } else {
                elapsed -= localDuration;

                // Fractional-sample edge smoothing limits aliasing.
                output = magnitude - ((magnitude * (int32_t)elapsed) >> 15);
                magnitude = -magnitude;
            }

            block->data[i] = (int16_t)output;
        }
    }

    transmit(block);
    release(block);
}
