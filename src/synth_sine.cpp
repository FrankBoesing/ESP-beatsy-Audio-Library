#include <math.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include "synth_sine.h"
#include "sine_table.h"
#include "utility/dspinst.h"
extern "C" {
extern const int16_t AudioWaveformSine[257];
}


OSPEED
void AudioSynthWaveformSine::update() {
    const uint32_t increment = phase_increment;

    // Keep phase progression independent of block allocation.
    if (magnitude == 0) {
        phase_accumulator += increment * (uint32_t)(AUDIO_BLOCK_SAMPLES);
        return;
    }

    audio_block_t *block = allocate();

    if (!block) {
        phase_accumulator += increment * (uint32_t)(AUDIO_BLOCK_SAMPLES);
        return;
    }

    uint32_t phase = phase_accumulator;

    AudioSineTable::generate(block->data, phase, increment, AUDIO_BLOCK_SAMPLES, magnitude);

    phase_accumulator = phase;

    transmit(block);
    release(block);
}

// Convert the 32-bit phase accumulator to a signed phase,
// then to radians. This avoids a discontinuity at the wraparound.
static inline float sineHiresPhaseToRadians(uint32_t phase) {
    int32_t signedPhase;
    memcpy(&signedPhase, &phase, sizeof(signedPhase));
    constexpr float PHASE_TO_RADIANS = 6.2831853071795864769f / 4294967296.0f;
    return (float)(signedPhase) * PHASE_TO_RADIANS;
}

// Convert normalized float audio to signed Q1.31.
// Clamp explicitly to avoid an out-of-range float-to-int conversion.
static inline int32_t sineHiresFloatToQ31(float value) {
    if (value >= 1.0f) {
        return INT32_MAX;
    }

    if (value <= -1.0f) {
        return INT32_MIN;
    }

    return (int32_t)(value * 2147483648.0f);
}

OSPEED
void AudioSynthWaveformSineHires::update() {
    const uint32_t increment = phase_increment;
    const int32_t mag = magnitude;

    uint32_t phase = phase_accumulator;

    // Preserve phase progression when output is muted.
    if (mag == 0) {
        phase_accumulator = phase + increment * (uint32_t)(AUDIO_BLOCK_SAMPLES);
        return;
    }

    // Hires encodes one 32-bit sample in two 16-bit output streams.
    audio_block_t *msw = allocate();

    if (!msw) {
        phase_accumulator = phase + increment * (uint32_t)(AUDIO_BLOCK_SAMPLES);
        return;
    }

    audio_block_t *lsw = allocate();

    if (!lsw) {
        release(msw);

        phase_accumulator = phase + increment * (uint32_t)(AUDIO_BLOCK_SAMPLES);
        return;
    }

    // magnitude is Q16: 65536 = unity gain.
    const float amplitude = (float)(mag) * (1.0f / 65536.0f);

    for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        const float radians = sineHiresPhaseToRadians(phase);

        // Hardware-assisted float arithmetic where supported.
        const float sineValue = sinf(radians) * amplitude;

        // Convert to signed 32-bit Q1.31.
        const int32_t value = sineHiresFloatToQ31(sineValue);

        // Output 0: upper 16 bits.
        msw->data[i] = (int16_t)(value >> 16);

        // Output 1: lower 16 bits, preserving the raw bit pattern.
        const uint16_t lowWord = (uint16_t)((uint32_t)(value));

        memcpy(&lsw->data[i], &lowWord, sizeof(lowWord));

        phase += increment;
    }

    phase_accumulator = phase;

    transmit(msw, 0);
    release(msw);

    transmit(lsw, 1);
    release(lsw);
}

OSPEED
void AudioSynthWaveformSineModulated::update() {
    audio_block_t *modinput = receiveReadOnly();
    audio_block_t *block = allocate();
    uint32_t phase = phase_accumulator;
    const uint32_t increment = phase_increment;
    const int32_t mag = magnitude;

    // Match the Teensy phase progression even when no output block is available.
    if (!block) {
        if (modinput) {
            for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
                const int32_t modulationQ16 = (int32_t)modinput->data[i] * 65536;
                const int32_t modulationIncrement =
                    multiply_32x32_rshift32((int32_t)increment, modulationQ16) * 2;
                phase += increment + (uint32_t)modulationIncrement;
            }
            release(modinput);
        } else {
            phase += increment * (uint32_t)AUDIO_BLOCK_SAMPLES;
        }

        phase_accumulator = phase;
        return;
    }

    for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        const uint32_t index = phase >> 24;
        int32_t val1 = AudioWaveformSine[index];
        int32_t val2 = AudioWaveformSine[index + 1];
        const uint32_t scale = (phase >> 8) & 0xFFFF;

        val2 *= (int32_t)scale;
        val1 *= (int32_t)(0x10000 - scale);
        block->data[i] = (int16_t)multiply_32x32_rshift32(val1 + val2, mag);

        if (modinput) {
            const int32_t modulationQ16 = (int32_t)modinput->data[i] * 65536;
            const int32_t modulationIncrement =
                multiply_32x32_rshift32((int32_t)increment, modulationQ16) * 2;
            phase += increment + (uint32_t)modulationIncrement;
        } else {
            phase += increment;
        }
    }

    phase_accumulator = phase;

    if (modinput) {
        release(modinput);
    }

    transmit(block);
    release(block);
}
