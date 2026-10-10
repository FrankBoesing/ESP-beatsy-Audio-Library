
/*
 * Audio Library for Teensy 3.X
 * Copyright (c) 2014, Paul Stoffregen
 *
 * ESP32 adaptation for ESP-beatsy Audio Library.
 */

#include <Arduino.h>
#include <stdint.h>

#include "analyze_rms.h"
#include "utility/sqrt_integer.h"

bool AudioAnalyzeRMS::available(void) {
    AudioStream::disableUpdates();
    const bool result = count > 0;
    AudioStream::enableUpdates();
    return result;
}

float AudioAnalyzeRMS::read(void) {
    // Atomically retrieve and reset the accumulated measurement.
    AudioStream::disableUpdates();

    const int64_t sum = accum;
    const uint32_t blocks = count;

    accum = 0;
    count = 0;

    AudioStream::enableUpdates();

    if (blocks == 0 || sum <= 0) {
        return 0.0f;
    }

    const uint64_t numSamples = (uint64_t)AUDIO_BLOCK_SAMPLES * blocks;

    // Mean square. The result cannot exceed 32768^2 for valid
    // int16_t input samples and therefore fits into uint32_t.
    const uint32_t meanSquare = (uint32_t)((uint64_t)(sum) / numSamples);

    // sqrt_uint32() uses __builtin_clz() and must not receive zero.
    if (meanSquare == 0) return 0.0f;

    // Integer square root; no sqrtf() required.
    return (float)sqrt_uint32(meanSquare) / 32767.0f;
}

void AudioAnalyzeRMS::update(void) {
    audio_block_t *block = receiveReadOnly();

    if (block == nullptr) {
        // Match the original Teensy behavior: a missing input block
        // contributes one silent block to the measurement interval.
        ++count;
        return;
    }

    int64_t sum = accum;
    const int16_t *p = block->data;

    for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        const int32_t sample = p[i];

        // The square fits in int32_t, including (-32768)^2.
        sum += sample * sample;
    }

    accum = sum;
    ++count;

    release(block);
}
