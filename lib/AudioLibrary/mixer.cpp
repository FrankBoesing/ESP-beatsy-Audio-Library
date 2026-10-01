/*
 * Audio Library for Teensy / ESP32
 * AudioMixer4 and AudioAmplifier
 *
 * ESP32 implementation:
 * - 32-bit intermediate arithmetic
 * - saturation only at the final 16-bit output
 * - no ARM-specific DSP instructions
 */

#include <Arduino.h>
#include "mixer.h"
#include "utility/dspinst.h"

#pragma optimize_for_speed

#define MULTI_UNITYGAIN 65536

// Apply gain to one sample.
//
// The audio sample is 16-bit, the gain is Q16.16.
// The multiplication is performed in 64-bit so that the full
// intermediate product cannot overflow before the shift.
static inline int32_t applyGain32(int16_t sample, int32_t mult) {
    return (int32_t)(((int64_t)sample * mult) >> 16);
}

// Apply gain to a complete block.
// The result intentionally remains 32-bit until it is saturated
// back to the 16-bit audio representation.
static void applyGain(int16_t *data, int32_t mult) {
    const int16_t *end = data + AUDIO_BLOCK_SAMPLES;

    if (mult == MULTI_UNITYGAIN) {
        return;
    }

#pragma GCC unroll 4
    do {
        int32_t value = applyGain32(*data, mult);
        *data++ = saturate16(value);
    } while (data < end);
}

// Add a gain-scaled source block to an existing destination block.
//
// The addition is performed in 32-bit arithmetic. Saturation happens
// only after the complete sample value has been calculated.
static void applyGainThenAdd(int16_t *dst, const int16_t *src, int32_t mult) {
    const int16_t *end = dst + AUDIO_BLOCK_SAMPLES;

    if (mult == MULTI_UNITYGAIN) {
#pragma GCC unroll 4
        do {
            int32_t value = (int32_t)*dst + (int32_t)*src++;
            *dst++ = saturate16(value);
        } while (dst < end);
        return;
    }

#pragma GCC unroll 4
    do {
        int32_t value = (int32_t)*dst + applyGain32(*src++, mult);
        *dst++ = saturate16(value);
    } while (dst < end);
}

void AudioMixer4::update(void) {
    audio_block_t *in;
    audio_block_t *out = nullptr;

    for (unsigned int channel = 0; channel < 4; channel++) {
        const int32_t mult = multiplier[channel];

        // NEU: Wenn der Kanal stumm ist, Eingang verwerfen und CPU sparen
        if (mult == 0) {
            in = receiveReadOnly(channel);
            if (in) {
                release(in);
            }
            continue;
        }

        if (!out) {
            // The first available input becomes the destination block.
            out = receiveWritable(channel);

            if (out) {
                if (mult != MULTI_UNITYGAIN) {
                    applyGain(out->data, mult);
                }
            }
        } else {
            // Further inputs are mixed into the existing destination.
            in = receiveReadOnly(channel);

            if (in) {
                applyGainThenAdd(out->data, in->data,
                                 mult); // mult wird direkt genutzt
                release(in);
            }
        }
    }

    if (out) {
        transmit(out);
        release(out);
    }
}

void AudioAmplifier::update(void) {
    audio_block_t *block;
    const int32_t mult = multiplier;

    if (mult == 0) {
        // Zero gain: discard the input.
        block = receiveReadOnly(0);
        if (block) {
            release(block);
        }
    } else if (mult == MULTI_UNITYGAIN) {
        // Unity gain: no sample calculation required.
        block = receiveReadOnly(0);

        if (block) {
            transmit(block);
            release(block);
        }
    } else {
        // Apply gain and saturate the result to 16-bit.
        block = receiveWritable(0);

        if (block) {
            applyGain(block->data, mult);
            transmit(block);
            release(block);
        }
    }
}
