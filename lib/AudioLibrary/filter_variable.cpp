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

#include "filter_variable.h"

#include <math.h>
#include <stdint.h>

#include "utility/dspinst.h"

// Chamberlin state-variable filter with 2x oversampling.
// The portable dspinst.h implementation supplies the multiply/saturation
// helpers used by the original Teensy fixed-point implementation.

// The multiply helper returns the upper 32 bits of the product, rounded.
// Multiplying by four gives the original Q30 scaling.
#define MULT(a, b) (multiply_32x32_rshift32_rounded((a), (b)) * 4)

AudioFilterStateVariable::AudioFilterStateVariable()
    : AudioStream(2, inputQueueArray) {
    frequency(1000.0f);
    octaveControl(1.0f);
    resonance(0.707f);
}

void AudioFilterStateVariable::frequency(float freq) {
    if (!isfinite(freq)) {
        return;
    }
    if (freq < 20.0f) {
        freq = 20.0f;
    } else if (freq > AUDIO_SAMPLE_RATE_EXACT / 2.5f) {
        freq = AUDIO_SAMPLE_RATE_EXACT / 2.5f;
    }

    const float scale = 3.141592654f / (AUDIO_SAMPLE_RATE_EXACT * 2.0f);
    const int32_t fcenter = (int32_t)((freq * scale) * 2147483647.0f);
    const int32_t fmult = (int32_t)(sinf(freq * scale) * 2147483647.0f);

    portENTER_CRITICAL(&settingsMux);
    settings.fcenter = fcenter;
    settings.fmult = fmult;
    portEXIT_CRITICAL(&settingsMux);
}

void AudioFilterStateVariable::resonance(float q) {
    if (!isfinite(q)) {
        return;
    }
    if (q < 0.7f) {
        q = 0.7f;
    } else if (q > 5.0f) {
        q = 5.0f;
    }

    // Q2.30 damping coefficient.
    const int32_t damping = (int32_t)((1.0f / q) * 1073741824.0f);

    portENTER_CRITICAL(&settingsMux);
    settings.damping = damping;
    portEXIT_CRITICAL(&settingsMux);
}

void AudioFilterStateVariable::octaveControl(float octaves) {
    if (!isfinite(octaves)) {
        return;
    }
    if (octaves < 0.0f) {
        octaves = 0.0f;
    } else if (octaves > 6.9999f) {
        octaves = 6.9999f;
    }

    const int32_t octaveMult = (int32_t)(octaves * 4096.0f);

    portENTER_CRITICAL(&settingsMux);
    settings.octaveMult = octaveMult;
    portEXIT_CRITICAL(&settingsMux);
}

OSPEED
void AudioFilterStateVariable::update_fixed(
    const int16_t *in, int16_t *lp, int16_t *bp, int16_t *hp,
    const Parameters &parameters) {

    const int16_t *end = in + AUDIO_BLOCK_SAMPLES;
    int32_t input;
    int32_t inputprev = state_inputprev;
    int32_t lowpass = state_lowpass;
    int32_t bandpass = state_bandpass;
    int32_t highpass;
    int32_t lowpasstmp;
    int32_t bandpasstmp;
    int32_t highpasstmp;
    const int32_t fmult = parameters.fmult;
    const int32_t damp = parameters.damping;

    do {
        // Multiplication rather than left-shifting a negative sample avoids
        // undefined signed-shift behaviour while retaining the original scale.
        input = (int32_t)(*in++) * 4096;

        lowpass = lowpass + MULT(fmult, bandpass);
        highpass = ((input + inputprev) >> 1) - lowpass - MULT(damp, bandpass);
        inputprev = input;
        bandpass = bandpass + MULT(fmult, highpass);

        lowpasstmp = lowpass;
        bandpasstmp = bandpass;
        highpasstmp = highpass;

        // Second internal step: 2x oversampling.
        lowpass = lowpass + MULT(fmult, bandpass);
        highpass = input - lowpass - MULT(damp, bandpass);
        bandpass = bandpass + MULT(fmult, highpass);

        lowpasstmp = signed_saturate_rshift(lowpass + lowpasstmp, 16, 13);
        bandpasstmp = signed_saturate_rshift(bandpass + bandpasstmp, 16, 13);
        highpasstmp = signed_saturate_rshift(highpass + highpasstmp, 16, 13);

        *lp++ = (int16_t)lowpasstmp;
        *bp++ = (int16_t)bandpasstmp;
        *hp++ = (int16_t)highpasstmp;
    } while (in < end);

    state_inputprev = inputprev;
    state_lowpass = lowpass;
    state_bandpass = bandpass;
}

OSPEED
void AudioFilterStateVariable::update_variable(
    const int16_t *in, const int16_t *ctl,
    int16_t *lp, int16_t *bp, int16_t *hp,
    const Parameters &parameters) {

    const int16_t *end = in + AUDIO_BLOCK_SAMPLES;
    int32_t input;
    int32_t inputprev = state_inputprev;
    int32_t lowpass = state_lowpass;
    int32_t bandpass = state_bandpass;
    int32_t highpass;
    int32_t lowpasstmp;
    int32_t bandpasstmp;
    int32_t highpasstmp;
    const int32_t fcenter = parameters.fcenter;
    const int32_t octaveMult = parameters.octaveMult;
    const int32_t damp = parameters.damping;

    int32_t fmult;
    int32_t control;
    int32_t n;

    do {
        // Control is signed Q15. The 12 fractional bits in octaveMult convert
        // it to an exponent measured in octaves.
        control = (int32_t)(*ctl++) * octaveMult;
        n = control & 0x7FFFFFF;

#ifdef IMPROVE_EXPONENTIAL_ACCURACY
        // Optional higher-accuracy exp2 polynomial from the original source.
        const int32_t x = n * 8;
        n = multiply_accumulate_32x32_rshift32_rounded(536870912, x, 1494202713);
        const int32_t sq = multiply_32x32_rshift32_rounded(x, x);
        n = multiply_accumulate_32x32_rshift32_rounded(n, sq, 1934101615);
        n = n + (multiply_32x32_rshift32_rounded(
            sq, multiply_32x32_rshift32_rounded(x, 1358044250)) * 2);
        n = n * 2;
#else
        // Fast exp2 approximation by Laurent de Soras, as used by Teensy.
        n = (n + 134217728) * 8;
        n = multiply_32x32_rshift32_rounded(n, n);
        n = multiply_32x32_rshift32_rounded(n, 715827883) * 8;
        n = n + 715827882;
#endif

        n = n >> (6 - (control >> 27));
        fmult = multiply_32x32_rshift32_rounded(fcenter, n);
        if (fmult > 5378279) {
            fmult = 5378279;
        }
        fmult = fmult * 256;

#ifdef IMPROVE_HIGH_FREQUENCY_ACCURACY
        // Optional high-frequency correction from the original implementation.
        fmult = (multiply_32x32_rshift32_rounded(fmult, 2145892402) +
                 multiply_32x32_rshift32_rounded(
                     multiply_32x32_rshift32_rounded(fmult, fmult),
                     multiply_32x32_rshift32_rounded(fmult, -1383276101))) * 2;
#endif

        input = (int32_t)(*in++) * 4096;

        lowpass = lowpass + MULT(fmult, bandpass);
        highpass = ((input + inputprev) >> 1) - lowpass - MULT(damp, bandpass);
        inputprev = input;
        bandpass = bandpass + MULT(fmult, highpass);

        lowpasstmp = lowpass;
        bandpasstmp = bandpass;
        highpasstmp = highpass;

        // Second internal step: 2x oversampling.
        lowpass = lowpass + MULT(fmult, bandpass);
        highpass = input - lowpass - MULT(damp, bandpass);
        bandpass = bandpass + MULT(fmult, highpass);

        lowpasstmp = signed_saturate_rshift(lowpass + lowpasstmp, 16, 13);
        bandpasstmp = signed_saturate_rshift(bandpass + bandpasstmp, 16, 13);
        highpasstmp = signed_saturate_rshift(highpass + highpasstmp, 16, 13);

        *lp++ = (int16_t)lowpasstmp;
        *bp++ = (int16_t)bandpasstmp;
        *hp++ = (int16_t)highpasstmp;
    } while (in < end);

    state_inputprev = inputprev;
    state_lowpass = lowpass;
    state_bandpass = bandpass;
}

OSPEED
void AudioFilterStateVariable::update() {
    audio_block_t *inputBlock = receiveReadOnly(0);
    audio_block_t *controlBlock = receiveReadOnly(1);
    audio_block_t *lowpassBlock = nullptr;
    audio_block_t *bandpassBlock = nullptr;
    audio_block_t *highpassBlock = nullptr;

    if (inputBlock == nullptr) {
        if (controlBlock != nullptr) {
            release(controlBlock);
        }
        return;
    }

    lowpassBlock = allocate();
    if (lowpassBlock == nullptr) {
        release(inputBlock);
        if (controlBlock != nullptr) {
            release(controlBlock);
        }
        return;
    }

    bandpassBlock = allocate();
    if (bandpassBlock == nullptr) {
        release(inputBlock);
        release(lowpassBlock);
        if (controlBlock != nullptr) {
            release(controlBlock);
        }
        return;
    }

    highpassBlock = allocate();
    if (highpassBlock == nullptr) {
        release(inputBlock);
        release(lowpassBlock);
        release(bandpassBlock);
        if (controlBlock != nullptr) {
            release(controlBlock);
        }
        return;
    }

    Parameters parameters;
    portENTER_CRITICAL(&settingsMux);
    parameters = settings;
    portEXIT_CRITICAL(&settingsMux);

    if (controlBlock != nullptr) {
        update_variable(inputBlock->data, controlBlock->data,
                        lowpassBlock->data, bandpassBlock->data,
                        highpassBlock->data, parameters);
        release(controlBlock);
    } else {
        update_fixed(inputBlock->data,
                     lowpassBlock->data, bandpassBlock->data,
                     highpassBlock->data, parameters);
    }

    release(inputBlock);

    transmit(lowpassBlock, 0);
    release(lowpassBlock);

    transmit(bandpassBlock, 1);
    release(bandpassBlock);

    transmit(highpassBlock, 2);
    release(highpassBlock);
}
