#include "filter_dcblock.h"

#include <cmath>
#include <cstdint>
#include <limits>

namespace {
constexpr double DCBLOCK_PI = 3.14159265358979323846;
constexpr int64_t Q31_HALF = int64_t{1} << 30;
constexpr double Q31_SCALE = 2147483648.0;

static inline int64_t multiplyQ31Rounded(int32_t coefficient, int32_t value) {
    const int64_t product = static_cast<int64_t>(coefficient) * value;

    if (product >= 0) {
        return (product + Q31_HALF) >> 31;
    }

    return -(((-product) + Q31_HALF) >> 31);
}
}  // namespace

AudioFilterDCBlock::AudioFilterDCBlock()
    : AudioStream(1, inputQueueArray) {}

void AudioFilterDCBlock::frequency(float frequency) {
    if (!std::isfinite(frequency) || frequency <= 0.0f) {
        return;
    }

    portENTER_CRITICAL(&_configMux);
    _cutoffFrequency = frequency;
    portEXIT_CRITICAL(&_configMux);
}

OSPEED
void AudioFilterDCBlock::update() {
    audio_block_t *block = receiveWritable();

    if (block == nullptr) {
        return;
    }

    const float currentSampleRate = sampleRate();
    if (!std::isfinite(currentSampleRate) || currentSampleRate <= 0.0f) {
        release(block);
        return;
    }

    float cutoffFrequency;
    portENTER_CRITICAL(&_configMux);
    cutoffFrequency = _cutoffFrequency;
    portEXIT_CRITICAL(&_configMux);

    // Keep enough distance from both zero and Nyquist to avoid a pole that
    // quantizes to exactly one in Q31 or an unnecessarily high cutoff.
    if (cutoffFrequency < 0.1f) {
        cutoffFrequency = 0.1f;
    }
    const float maxFrequency = currentSampleRate * 0.45f;
    if (cutoffFrequency > maxFrequency) {
        cutoffFrequency = maxFrequency;
    }

    // y[n] = x[n] - x[n-1] + R*y[n-1], R = exp(-2*pi*fc/fs).
    // Calculate the coefficient once per block, so a sample-rate change is
    // reflected automatically without doing transcendental math per sample.
    const double pole = std::exp(
        -2.0 * DCBLOCK_PI * static_cast<double>(cutoffFrequency) /
        static_cast<double>(currentSampleRate));
    int64_t poleQ31Wide = static_cast<int64_t>(pole * Q31_SCALE + 0.5);
    if (poleQ31Wide > std::numeric_limits<int32_t>::max()) {
        poleQ31Wide = std::numeric_limits<int32_t>::max();
    } else if (poleQ31Wide < 0) {
        poleQ31Wide = 0;
    }
    const int32_t poleQ31 = static_cast<int32_t>(poleQ31Wide);

    int32_t previousInput = _previousInput;
    int32_t previousOutput = _previousOutput;

    for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        const int32_t input = block->data[i];
        const int64_t output = static_cast<int64_t>(input) - previousInput +
                               multiplyQ31Rounded(poleQ31, previousOutput);

        previousInput = input;
        if (output > std::numeric_limits<int32_t>::max()) {
            previousOutput = std::numeric_limits<int32_t>::max();
        } else if (output < std::numeric_limits<int32_t>::min()) {
            previousOutput = std::numeric_limits<int32_t>::min();
        } else {
            // Retain the internal filter state before 16-bit output clipping.
            previousOutput = static_cast<int32_t>(output);
        }

        if (output > INT16_MAX) {
            block->data[i] = INT16_MAX;
        } else if (output < INT16_MIN) {
            block->data[i] = INT16_MIN;
        } else {
            block->data[i] = static_cast<int16_t>(output);
        }
    }

    _previousInput = previousInput;
    _previousOutput = previousOutput;

    transmit(block);
    release(block);
}
