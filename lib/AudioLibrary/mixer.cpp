/*
 * Audio Library for Teensy / ESP32
 * AudioMixer4, AudioMixerN and AudioAmplifier
 *
 * AudioMixer4:
 *   Existing implementation is intentionally unchanged.
 *
 * AudioMixerN:
 *   - 4..32 inputs
 *   - float gain
 *   - float accumulation
 *   - saturation only once at final 16-bit output
 */

#include <Arduino.h>

#include "mixer.h"
#include "utility/dspinst.h"

#pragma optimize_for_speed

#define MULTI_UNITYGAIN 65536


// ============================================================================
// Existing AudioMixer4 / AudioAmplifier helpers
// ============================================================================

// Apply gain to one sample.
//
// The audio sample is 16-bit, the gain is Q16.16.
// The multiplication is performed in 64-bit so that the full
// intermediate product cannot overflow before the shift.
static inline int32_t applyGain32(int16_t sample, int32_t mult)
{
    return (int32_t)(((int64_t)sample * mult) >> 16);
}


// Apply gain to a complete block.
static void applyGain(int16_t *data, int32_t mult)
{
    const int16_t *end = data + AUDIO_BLOCK_SAMPLES;

    if (mult == MULTI_UNITYGAIN) {
        return;
    }

#pragma GCC unroll 4
    do {
        const int32_t value = applyGain32(*data, mult);
        *data++ = saturate16(value);
    } while (data < end);
}


// Add a gain-scaled source block to an existing destination block.
//
// Existing AudioMixer4 behavior.
static void applyGainThenAdd(
    int16_t *dst,
    const int16_t *src,
    int32_t mult)
{
    const int16_t *end = dst + AUDIO_BLOCK_SAMPLES;

    if (mult == MULTI_UNITYGAIN) {

#pragma GCC unroll 4
        do {
            const int32_t value =
                (int32_t)*dst + (int32_t)*src++;

            *dst++ = saturate16(value);

        } while (dst < end);

        return;
    }

#pragma GCC unroll 4
    do {
        const int32_t value =
            (int32_t)*dst + applyGain32(*src++, mult);

        *dst++ = saturate16(value);

    } while (dst < end);
}


// ============================================================================
// AudioMixerN
// ============================================================================

AudioMixerN::AudioMixerN(unsigned int channels)
    : AudioStream(
          (channels < 4)
              ? 4
              : (channels > AUDIO_MIXERN_MAX_INPUTS
                     ? AUDIO_MIXERN_MAX_INPUTS
                     : (unsigned char)channels),
          inputQueueArray),
      num_inputs(
          (channels < 4)
              ? 4
              : (channels > AUDIO_MIXERN_MAX_INPUTS
                     ? AUDIO_MIXERN_MAX_INPUTS
                     : channels))
{
    for (unsigned int i = 0;
         i < AUDIO_MIXERN_MAX_INPUTS;
         ++i)
    {
        gains[i] = 1.0f;
        inputQueueArray[i] = nullptr;
    }
}


// ============================================================================
// Set gain
// ============================================================================

void AudioMixerN::gain(
    unsigned int channel,
    float gain)
{
    if (channel >= num_inputs)
        return;

    if (gain > 32767.0f)
        gain = 32767.0f;
    else if (gain < -32767.0f)
        gain = -32767.0f;

    gains[channel] = gain;
}


// ============================================================================
// AudioMixerN update
// ============================================================================

void AudioMixerN::update(void)
{
    // ------------------------------------------------------------------------
    // Active input lists.
    //
    // Only actually received inputs with non-zero gain are put into these
    // arrays. This keeps the inner sample loop free of pointer/null tests.
    // ------------------------------------------------------------------------

    audio_block_t *inputs[AUDIO_MIXERN_MAX_INPUTS];
    const float *activeGains[AUDIO_MIXERN_MAX_INPUTS];

    unsigned int activeInputs = 0;


    // ------------------------------------------------------------------------
    // Receive inputs.
    // ------------------------------------------------------------------------

    for (unsigned int channel = 0;
         channel < num_inputs;
         ++channel)
    {
        audio_block_t *in =
            receiveReadOnly(channel);

        if (!in)
            continue;

        const float gain =
            gains[channel];

        if (gain == 0.0f) {
            release(in);
            continue;
        }

        inputs[activeInputs] = in;
        activeGains[activeInputs] = &gains[channel];

        ++activeInputs;
    }


    // ------------------------------------------------------------------------
    // No active input.
    // ------------------------------------------------------------------------

    if (activeInputs == 0)
        return;


    // ------------------------------------------------------------------------
    // Allocate output.
    // ------------------------------------------------------------------------

    audio_block_t *out =
        allocate();

    if (!out) {

        for (unsigned int i = 0;
             i < activeInputs;
             ++i)
        {
            release(inputs[i]);
        }

        return;
    }


    // ------------------------------------------------------------------------
    // Float mixer.
    //
    // No saturation occurs inside the channel loop.
    // The complete sum remains available as float.
    // ------------------------------------------------------------------------

    for (int i = 0;
         i < AUDIO_BLOCK_SAMPLES;
         ++i)
    {
        float sum = 0.0f;

#pragma GCC unroll 4
        for (unsigned int channel = 0;
             channel < activeInputs;
             ++channel)
        {
            sum +=
                (float)inputs[channel]->data[i] *
                *activeGains[channel];
        }


        // --------------------------------------------------------------------
        // ONE saturation operation at the final PCM16 boundary.
        // --------------------------------------------------------------------

        if (sum > 32767.0f)
            sum = 32767.0f;
        else if (sum < -32768.0f)
            sum = -32768.0f;

        out->data[i] = (int16_t)sum;
    }


    // ------------------------------------------------------------------------
    // Release inputs.
    // ------------------------------------------------------------------------

    for (unsigned int i = 0;
         i < activeInputs;
         ++i)
    {
        release(inputs[i]);
    }


    // ------------------------------------------------------------------------
    // Send output.
    // ------------------------------------------------------------------------

    transmit(out);
    release(out);
}


// ============================================================================
// Existing AudioMixer4 implementation
// ============================================================================

void AudioMixer4::update(void)
{
    audio_block_t *in;
    audio_block_t *out = nullptr;

    for (unsigned int channel = 0;
         channel < 4;
         channel++)
    {
        const int32_t mult =
            multiplier[channel];

        // NEU: Wenn der Kanal stumm ist,
        // Eingang verwerfen und CPU sparen.

        if (mult == 0) {

            in =
                receiveReadOnly(channel);

            if (in) {
                release(in);
            }

            continue;
        }


        if (!out) {

            // The first available input becomes
            // the destination block.

            out =
                receiveWritable(channel);

            if (out) {

                if (mult != MULTI_UNITYGAIN) {
                    applyGain(
                        out->data,
                        mult
                    );
                }
            }

        } else {

            // Further inputs are mixed into
            // the existing destination.

            in =
                receiveReadOnly(channel);

            if (in) {

                applyGainThenAdd(
                    out->data,
                    in->data,
                    mult
                );

                release(in);
            }
        }
    }


    if (out) {

        transmit(out);
        release(out);
    }
}


// ============================================================================
// Existing AudioAmplifier implementation
// ============================================================================

void AudioAmplifier::update(void)
{
    audio_block_t *block;

    const int32_t mult =
        multiplier;


    if (mult == 0) {

        // Zero gain:
        // discard input.

        block =
            receiveReadOnly(0);

        if (block) {
            release(block);
        }

    } else if (mult == MULTI_UNITYGAIN) {

        // Unity gain:
        // no sample calculation required.

        block =
            receiveReadOnly(0);

        if (block) {

            transmit(block);
            release(block);
        }

    } else {

        // Apply gain and saturate
        // the result to 16-bit.

        block =
            receiveWritable(0);

        if (block) {

            applyGain(
                block->data,
                mult
            );

            transmit(block);
            release(block);
        }
    }
}
