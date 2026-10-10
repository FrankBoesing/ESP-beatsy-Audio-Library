/*
 * Audio Library for Teensy, Ladder Filter
 * Copyright (c) 2021, Richard van Hoesel
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
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "filter_ladder.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#define MOOG_PI 3.14159265358979323846f
#define MAX_RESONANCE 1.8f
#define MAX_FREQUENCY (AUDIO_SAMPLE_RATE_EXACT * 0.425f)

const float AudioFilterLadder::interpolation_coeffs[AudioFilterLadder::INTERPOLATION_TAPS] = {
    -14.30851541590154240E-6f,  0.001348560352009071f, 0.004029285548698377f, 0.007644563345368599f,
     0.010936856250494802f,    0.011982063548666887f, 0.008882946305001046f, 0.0008266598116471556070f,
    -0.011008071930708746f,   -0.023014151355548934f,-0.029736402750934567f,-0.025405787911977455f,
    -0.006012006772274640f,    0.028729626071574525f, 0.074466890595619062f, 0.122757573409695370f,
     0.163145421379242955f,    0.186152844567746417f, 0.186152844567746417f, 0.163145421379242955f,
     0.122757573409695370f,    0.074466890595619062f, 0.028729626071574525f,-0.006012006772274640f,
    -0.025405787911977455f,   -0.029736402750934567f,-0.023014151355548934f,-0.011008071930708746f,
     0.0008266598116471556070f,0.008882946305001046f, 0.011982063548666887f, 0.010936856250494802f,
     0.007644563345368599f,    0.004029285548698377f, 0.001348560352009071f,-14.30851541590154240E-6f
};

AudioFilterLadder::AudioFilterLadder()
    : AudioStream(3, inputQueueArray) {
    computeCoefficients(baseFrequency, alpha, qAdjust);
}

void AudioFilterLadder::frequency(float fc) {
    if (!isfinite(fc)) {
        return;
    }

    portENTER_CRITICAL(&configMux);
    baseFrequency = fc;
    ++frequencyGeneration;
    portEXIT_CRITICAL(&configMux);
}

void AudioFilterLadder::resonance(float reson) {
    if (!isfinite(reson)) {
        return;
    }
    if (reson > MAX_RESONANCE) {
        reson = MAX_RESONANCE;
    } else if (reson < 0.0f) {
        reson = 0.0f;
    }

    portENTER_CRITICAL(&configMux);
    baseK = 4.0f * reson;
    portEXIT_CRITICAL(&configMux);
}

void AudioFilterLadder::octaveControl(float octaves) {
    if (!isfinite(octaves)) {
        return;
    }
    if (octaves > 7.0f) {
        octaves = 7.0f;
    } else if (octaves < 0.0f) {
        octaves = 0.0f;
    }

    portENTER_CRITICAL(&configMux);
    octaveScale = octaves / 32768.0f;
    portEXIT_CRITICAL(&configMux);
}

void AudioFilterLadder::passbandGain(float passbandgain) {
    if (!isfinite(passbandgain)) {
        return;
    }
    if (passbandgain > 0.5f) {
        passbandgain = 0.5f;
    } else if (passbandgain < 0.0f) {
        passbandgain = 0.0f;
    }

    portENTER_CRITICAL(&configMux);
    pbg = passbandgain;

    if (hostOverdrive > 1.0f) {
        overdrive = 1.0f + (hostOverdrive - 1.0f) * (1.0f - pbg);
    } else {
        overdrive = hostOverdrive;
        if (overdrive < 0.0f) {
            overdrive = 0.0f;
        }
    }
    portEXIT_CRITICAL(&configMux);
}

void AudioFilterLadder::inputDrive(float drv) {
    if (!isfinite(drv)) {
        return;
    }

    portENTER_CRITICAL(&configMux);
    hostOverdrive = drv;
    if (hostOverdrive > 1.0f) {
        if (hostOverdrive > 4.0f) {
            hostOverdrive = 4.0f;
        }
        // Maximum drive is 4.0 for pbg=0 and 2.5 for pbg=0.5.
        overdrive = 1.0f + (hostOverdrive - 1.0f) * (1.0f - pbg);
    } else {
        overdrive = hostOverdrive;
        if (overdrive < 0.0f) {
            overdrive = 0.0f;
        }
    }
    portEXIT_CRITICAL(&configMux);
}

void AudioFilterLadder::interpolationMethod(AudioFilterLadderInterpolation method) {
    portENTER_CRITICAL(&configMux);
    polyOn = (method == LADDER_FILTER_INTERPOLATION_FIR_POLY);
    portEXIT_CRITICAL(&configMux);
}

void AudioFilterLadder::computeCoefficients(float fc, float &newAlpha, float &newQAdjust) const {
    if (!isfinite(fc)) {
        fc = 5.0f;
    }
    if (fc > MAX_FREQUENCY) {
        fc = MAX_FREQUENCY;
    } else if (fc < 5.0f) {
        fc = 5.0f;
    }

    const float wc = fc * (2.0f * MOOG_PI /
                           ((float)INTERPOLATION * AUDIO_SAMPLE_RATE_EXACT));
    const float wc2 = wc * wc;

    newAlpha = 0.9892f * wc - 0.4324f * wc2 +
               0.1381f * wc * wc2 - 0.0202f * wc2 * wc2;
    newQAdjust = 1.006f + 0.0536f * wc - 0.095f * wc2 -
                 0.05f * wc2 * wc2;
}

float AudioFilterLadder::lpf(float sample, int stage) {
    float filtered = sample * (1.0f / 1.3f) +
                     (0.3f / 1.3f) * z0[stage] - z1[stage];
    filtered = filtered * alpha + z1[stage];
    z1[stage] = filtered;
    z0[stage] = sample;
    return filtered;
}

bool AudioFilterLadder::resonating() const {
    for (int i = 0; i < 4; ++i) {
        if (fabsf(z0[i]) > 0.0001f || fabsf(z1[i]) > 0.0001f) {
            return true;
        }
    }
    return false;
}

float AudioFilterLadder::fastExp2(float x) {
    float integerPart;
    float fraction = modff(x, &integerPart);
    fraction *= 0.693147f / 256.0f;
    fraction += 1.0f;
    fraction *= fraction;
    fraction *= fraction;
    fraction *= fraction;
    fraction *= fraction;
    fraction *= fraction;
    fraction *= fraction;
    fraction *= fraction;
    fraction *= fraction;
    return ldexpf(fraction, (int)integerPart);
}

float AudioFilterLadder::fastTanh(float x) {
    if (x > 3.0f) {
        return 1.0f;
    }
    if (x < -3.0f) {
        return -1.0f;
    }

    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

int16_t AudioFilterLadder::toPcm(float value) {
    const float scaled = value * 32768.0f;
    if (!isfinite(scaled)) {
        return 0;
    }
    if (scaled >= 32767.0f) {
        return INT16_MAX;
    }
    if (scaled <= -32768.0f) {
        return INT16_MIN;
    }
    return (int16_t)scaled;
}

// Portable equivalent of CMSIS arm_fir_interpolate_f32() for L=4, 36 taps.
// Coefficients remain in CMSIS time-reversed order; the phases are emitted
// from L-1 down to zero to preserve the original interpolation timing.
void AudioFilterLadder::interpolateBlock(const int16_t *input, float drive) {
    constexpr int phaseLength = INTERPOLATION_PHASE_LENGTH;

    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        // Match CMSIS state timing: only append the current sample before
        // calculating its four interpolated output samples. Copying the full
        // block first would make future input samples visible to the FIR.
        interpolationState[(phaseLength - 1) + i] =
            ((float)input[i] * drive * (float)INTERPOLATION) / 32768.0f;

        const float *state = interpolationState + i;

        for (int phase = INTERPOLATION; phase > 0; --phase) {
            float sum = 0.0f;
            const int coefficientPhase = phase - 1;

            for (int tap = 0; tap < phaseLength; ++tap) {
                sum += state[tap] * interpolation_coeffs[
                    coefficientPhase + tap * INTERPOLATION];
            }

            oversampled[i * INTERPOLATION + (INTERPOLATION - phase)] = sum;
        }
    }

    // Preserve the previous phaseLength-1 input samples for the next block.
    memmove(interpolationState,
            interpolationState + AUDIO_BLOCK_SAMPLES,
            (phaseLength - 1) * sizeof(float));
}

// Portable equivalent of CMSIS arm_fir_decimate_f32() for M=4, 36 taps.
// The full input is copied to state first, so oversampled[] can also be used
// as the output buffer without corrupting samples that have yet to be read.
void AudioFilterLadder::decimateBlock() {
    constexpr int tapHistory = INTERPOLATION_TAPS - 1;
    constexpr int inputSamples = AUDIO_BLOCK_SAMPLES * INTERPOLATION;

    memcpy(decimationState + tapHistory, oversampled,
           inputSamples * sizeof(float));

    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        const float *state = decimationState + i * INTERPOLATION;
        float sum = 0.0f;

        for (int tap = 0; tap < INTERPOLATION_TAPS; ++tap) {
            sum += state[tap] * interpolation_coeffs[tap];
        }

        oversampled[i] = sum;
    }

    // Keep the final tapHistory samples as history for the next call.
    memmove(decimationState,
            decimationState + inputSamples,
            tapHistory * sizeof(float));
}

OSPEED
void AudioFilterLadder::update() {
    audio_block_t *block = receiveWritable(0);
    audio_block_t *frequencyBlock = receiveReadOnly(1);
    audio_block_t *resonanceBlock = receiveReadOnly(2);

    if (block == nullptr) {
        if (resonating()) {
            // Continue processing with silence while the filter rings out.
            block = allocate();
        }

        if (block == nullptr) {
            if (frequencyBlock != nullptr) {
                release(frequencyBlock);
            }
            if (resonanceBlock != nullptr) {
                release(resonanceBlock);
            }
            return;
        }

        memset(block->data, 0, sizeof(block->data));
    }

    Config config;
    portENTER_CRITICAL(&configMux);
    config.baseFrequency = baseFrequency;
    config.k = baseK;
    config.octaveScale = octaveScale;
    config.passbandGain = pbg;
    config.overdrive = overdrive;
    config.frequencyGeneration = frequencyGeneration;
    config.polyOn = polyOn;
    portEXIT_CRITICAL(&configMux);

    // frequency() only changes the requested base frequency from task context.
    // Coefficients are recalculated here, in the audio task.
    if (config.frequencyGeneration != appliedFrequencyGeneration) {
        computeCoefficients(config.baseFrequency, alpha, qAdjust);
        appliedFrequencyGeneration = config.frequencyGeneration;
    }

    float totalResonance = config.k;
    const bool frequencyModulationActive = (frequencyBlock != nullptr);
    const bool resonanceModulationActive = (resonanceBlock != nullptr);

    if (config.polyOn) {
        interpolateBlock(block->data, config.overdrive);

        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            if (frequencyModulationActive) {
                const float octave = (float)frequencyBlock->data[i] * config.octaveScale;
                float cutoff = config.baseFrequency * fastExp2(octave);
                if (cutoff > MAX_FREQUENCY) {
                    cutoff = MAX_FREQUENCY;
                }
                computeCoefficients(cutoff, alpha, qAdjust);
            }

            if (resonanceModulationActive) {
                const float qMod = (float)resonanceBlock->data[i] * (1.0f / 32768.0f);
                totalResonance = config.k + 4.0f * qMod;
            }

            if (totalResonance > MAX_RESONANCE * 4.0f) {
                totalResonance = MAX_RESONANCE * 4.0f;
            } else if (totalResonance < 0.0f) {
                totalResonance = 0.0f;
            }

            for (int os = 0; os < INTERPOLATION; ++os) {
                const int index = i * INTERPOLATION + os;
                const float input = oversampled[index];
                float u = input -
                          (z1[3] - config.passbandGain * input) *
                              totalResonance * qAdjust;
                u = fastTanh(u);

                const float stage1 = lpf(u, 0);
                const float stage2 = lpf(stage1, 1);
                const float stage3 = lpf(stage2, 2);
                oversampled[index] = lpf(stage3, 3);
            }
        }

        decimateBlock();

        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            block->data[i] = toPcm(oversampled[i]);
        }
    } else {
        // Lower-CPU mode: linear interpolation between consecutive input samples.
        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            const float input =
                ((float)block->data[i] * config.overdrive) / 32768.0f;

            if (frequencyModulationActive) {
                const float octave = (float)frequencyBlock->data[i] * config.octaveScale;
                float cutoff = config.baseFrequency * fastExp2(octave);
                if (cutoff > MAX_FREQUENCY) {
                    cutoff = MAX_FREQUENCY;
                }
                computeCoefficients(cutoff, alpha, qAdjust);
            }

            if (resonanceModulationActive) {
                const float qMod = (float)resonanceBlock->data[i] * (1.0f / 32768.0f);
                totalResonance = config.k + 4.0f * qMod;
            }

            if (totalResonance > MAX_RESONANCE * 4.0f) {
                totalResonance = MAX_RESONANCE * 4.0f;
            } else if (totalResonance < 0.0f) {
                totalResonance = 0.0f;
            }

            float total = 0.0f;
            float interpolation = 0.0f;
            for (int os = 0; os < INTERPOLATION; ++os) {
                float u = (interpolation * oldInput +
                           (1.0f - interpolation) * input) -
                          (z1[3] - config.passbandGain * input) *
                              totalResonance * qAdjust;
                u = fastTanh(u);

                const float stage1 = lpf(u, 0);
                const float stage2 = lpf(stage1, 1);
                const float stage3 = lpf(stage2, 2);
                const float stage4 = lpf(stage3, 3);

                total += stage4 * (1.0f / (float)INTERPOLATION);
                interpolation += 1.0f / (float)INTERPOLATION;
            }

            oldInput = input;
            block->data[i] = toPcm(total);
        }
    }

    transmit(block);
    release(block);

    if (frequencyBlock != nullptr) {
        release(frequencyBlock);
    }
    if (resonanceBlock != nullptr) {
        release(resonanceBlock);
    }
}
