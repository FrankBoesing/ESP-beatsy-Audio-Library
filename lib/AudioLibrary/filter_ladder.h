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

// Huovilainen New Moog (HNM) model as per CMJ June 2006.
// Richard van Hoesel, v1.5. This port keeps the Teensy-compatible API and
// replaces CMSIS-DSP FIR calls with portable polyphase FIR routines.

#ifndef filter_ladder_h_
#define filter_ladder_h_

#include "AudioStream.h"

#include <stdint.h>

enum AudioFilterLadderInterpolation {
    LADDER_FILTER_INTERPOLATION_LINEAR,
    LADDER_FILTER_INTERPOLATION_FIR_POLY
};

class AudioFilterLadder : public AudioStream {
  public:
    AudioFilterLadder();

    void frequency(float FC);
    void resonance(float reson);
    void octaveControl(float octaves);
    void passbandGain(float passbandgain);
    void inputDrive(float drv);
    void interpolationMethod(AudioFilterLadderInterpolation im);
    void update() override;

  private:
    static constexpr int INTERPOLATION = 4;
    static constexpr int INTERPOLATION_TAPS = 36;
    static constexpr int INTERPOLATION_PHASE_LENGTH = INTERPOLATION_TAPS / INTERPOLATION;

    struct Config {
        float baseFrequency;
        float k;
        float octaveScale;
        float passbandGain;
        float overdrive;
        uint32_t frequencyGeneration;
        bool polyOn;
    };

    static const float interpolation_coeffs[INTERPOLATION_TAPS];

    static float fastExp2(float x);
    static float fastTanh(float x);
    static int16_t toPcm(float value);

    void computeCoefficients(float fc, float &alpha, float &qAdjust) const;
    float lpf(float sample, int stage);
    bool resonating() const;
    void interpolateBlock(const int16_t *input, float overdrive);
    void decimateBlock();

    audio_block_t *inputQueueArray[3] = {};

    // Four-stage nonlinear filter state. Accessed only by update().
    float alpha = 0.0f;
    float qAdjust = 1.0f;
    float z0[4] = {};
    float z1[4] = {};
    float oldInput = 0.0f;
    uint32_t appliedFrequencyGeneration = 1;

    // Working buffers and FIR histories are object members rather than large
    // stack arrays in update(), reducing peak audio-task stack consumption.
    float interpolationState[(AUDIO_BLOCK_SAMPLES - 1) + INTERPOLATION_PHASE_LENGTH] = {};
    float decimationState[(AUDIO_BLOCK_SAMPLES * INTERPOLATION - 1) + INTERPOLATION_TAPS] = {};
    float oversampled[AUDIO_BLOCK_SAMPLES * INTERPOLATION] = {};

    // Updated from control/task context and snapshotted once per audio block.
    portMUX_TYPE configMux = portMUX_INITIALIZER_UNLOCKED;
    float baseFrequency = 1000.0f;
    float baseK = 1.0f;
    float octaveScale = 1.0f / 32768.0f;
    float pbg = 0.5f;
    float hostOverdrive = 1.0f;
    float overdrive = 0.5f;
    uint32_t frequencyGeneration = 1;
    bool polyOn = true;
};

#endif
