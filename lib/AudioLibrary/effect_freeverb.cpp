/*
 * Freeverb for the Teensy Audio Library, adapted for ESP32.
 * Copyright (c) 2018, Paul Stoffregen, paul@pjrc.com
 *
 * This implementation preserves the Teensy Audio Library's fixed-point
 * algorithm, while making the delay-line storage dynamically allocated so
 * that it can use PSRAM on ESP32 boards.
 */

#include "effect_freeverb.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

static constexpr const char *TAG = "AudioEffectFreeverb";

static const uint16_t COMB_LENGTHS_MONO[8] = {
    1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617
};
static const uint16_t ALLPASS_LENGTHS_MONO[4] = {
    556, 441, 341, 225
};
static const uint16_t COMB_LENGTHS_RIGHT[8] = {
    1139, 1211, 1300, 1379, 1445, 1514, 1580, 1640
};
static const uint16_t ALLPASS_LENGTHS_RIGHT[4] = {
    579, 464, 364, 248
};

static constexpr size_t MONO_DELAY_SAMPLES = 12587;
static constexpr size_t STEREO_DELAY_SAMPLES = 25450;

// Freeverb's original fixed-point implementation rounds negative values
// toward zero before shifting, then clamps the result to int16_t.
static inline int16_t sat16_shift(int64_t value, unsigned int shift) {
    if (shift != 0U && value < 0) {
        value += ((int64_t)1 << shift) - 1;
    }
    value >>= shift;

    if (value > INT16_MAX) {
        return INT16_MAX;
    }
    if (value < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)value;
}

AudioEffectFreeverbBase::AudioEffectFreeverbBase(bool stereo)
    : AudioStream(1, inputQueueArray),
      delay_samples(stereo ? STEREO_DELAY_SAMPLES : MONO_DELAY_SAMPLES),
      stereo_mode(stereo) {
}

AudioEffectFreeverbBase::~AudioEffectFreeverbBase() {
    if (delay_memory != nullptr) {
        heap_caps_free(delay_memory);
        delay_memory = nullptr;
    }
}

void AudioEffectFreeverbBase::configureChannel(
    ChannelState &channel, size_t &offset,
    const uint16_t *comb_lengths, const uint16_t *allpass_lengths) {

    channel = ChannelState();

    for (size_t i = 0; i < 8; ++i) {
        channel.comb[i] = delay_memory + offset;
        channel.comb_length[i] = comb_lengths[i];
        offset += comb_lengths[i];
    }

    for (size_t i = 0; i < 4; ++i) {
        channel.allpass[i] = delay_memory + offset;
        channel.allpass_length[i] = allpass_lengths[i];
        offset += allpass_lengths[i];
    }
}

bool AudioEffectFreeverbBase::begin() {
    if (delay_memory != nullptr) {
        return true;
    }

    const size_t bytes = delay_samples * sizeof(int16_t);

    // Prefer internal RAM for predictable access latency. If the complete
    // delay-line pool does not fit, retry the pool in PSRAM as one allocation.
    delay_memory = (int16_t *)heap_caps_malloc(
        bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (delay_memory != nullptr) {
        using_psram = false;
    } else {
        delay_memory = (int16_t *)heap_caps_malloc(
            bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (delay_memory != nullptr) {
            using_psram = true;
            ESP_LOGD(TAG, "Internal RAM insufficient; using PSRAM for delay lines");
        } else {
            using_psram = false;
            if (!allocation_error_logged) {
                ESP_LOGE(TAG, "Unable to allocate Freeverb delay lines in internal RAM or PSRAM");
                allocation_error_logged = true;
            }
            return false;
        }
    }

    memset(delay_memory, 0, bytes);

    size_t offset = 0;
    configureChannel(channels[0], offset,
                     COMB_LENGTHS_MONO, ALLPASS_LENGTHS_MONO);

    if (stereo_mode) {
        configureChannel(channels[1], offset,
                         COMB_LENGTHS_RIGHT, ALLPASS_LENGTHS_RIGHT);
    }

    allocation_error_logged = false;
    return true;
}

void AudioEffectFreeverbBase::roomsize(float n) {
    if (!isfinite(n)) {
        return;
    }
    if (n > 1.0f) {
        n = 1.0f;
    } else if (n < 0.0f) {
        n = 0.0f;
    }

    const int32_t feedback = (int32_t)(n * 9175.04f) + 22937;
    portENTER_CRITICAL(&parameter_mux);
    comb_feedback = feedback;
    portEXIT_CRITICAL(&parameter_mux);
}

void AudioEffectFreeverbBase::damping(float n) {
    if (!isfinite(n)) {
        return;
    }
    if (n > 1.0f) {
        n = 1.0f;
    } else if (n < 0.0f) {
        n = 0.0f;
    }

    const int32_t damping1 = (int32_t)(n * 13107.2f);
    // Keep this as int32_t: Q15 value 32768 (unity) cannot be represented by
    // int16_t and would otherwise wrap to -32768 at damping(0.0f).
    const int32_t damping2 = 32768 - damping1;

    portENTER_CRITICAL(&parameter_mux);
    comb_damp1 = damping1;
    comb_damp2 = damping2;
    portEXIT_CRITICAL(&parameter_mux);
}

int16_t AudioEffectFreeverbBase::processSample(
    ChannelState &channel, int16_t input, int32_t feedback,
    int32_t damping1, int32_t damping2) {

    // Preserve the input/output scaling used by the original Teensy effect.
    input = sat16_shift((int32_t)input * 8738, 17);

    int32_t sum = 0;
    for (size_t i = 0; i < 8; ++i) {
        const uint16_t index = channel.comb_index[i];
        const int16_t bufout = channel.comb[i][index];
        sum += bufout;

        // These products stay in int32_t for the coefficient ranges accepted
        // by roomsize() and damping(); keeping them 32-bit avoids expensive
        // 64-bit multiplies in the per-sample inner loop.
        const int32_t damped = (int32_t)bufout * damping2 +
                               (int32_t)channel.comb_filter[i] * damping1;
        channel.comb_filter[i] = sat16_shift(damped, 15);

        const int32_t feedback_product =
            (int32_t)channel.comb_filter[i] * feedback;
        const int16_t feedback_sample = sat16_shift(feedback_product, 15);
        channel.comb[i][index] = sat16_shift(
            (int32_t)input + feedback_sample, 0);

        uint16_t next = (uint16_t)(index + 1U);
        if (next >= channel.comb_length[i]) {
            next = 0;
        }
        channel.comb_index[i] = next;
    }

    int16_t output = sat16_shift((int64_t)sum * 31457, 17);

    for (size_t i = 0; i < 4; ++i) {
        const uint16_t index = channel.allpass_index[i];
        const int16_t bufout = channel.allpass[i][index];

        // Match the original Freeverb allpass delay-line arithmetic.
        channel.allpass[i][index] =
            (int16_t)((int32_t)output + ((int32_t)bufout >> 1));
        output = sat16_shift((int32_t)bufout - output, 1);

        uint16_t next = (uint16_t)(index + 1U);
        if (next >= channel.allpass_length[i]) {
            next = 0;
        }
        channel.allpass_index[i] = next;
    }

    return sat16_shift((int64_t)output * 30, 0);
}

void AudioEffectFreeverbBase::passThroughOnAllocationFailure() {
    audio_block_t *block = receiveReadOnly();
    if (block == nullptr) {
        return;
    }

    if (stereo_mode) {
        transmit(block, 0);
        transmit(block, 1);
    } else {
        transmit(block, 0);
    }
    release(block);
}

AudioEffectFreeverb::AudioEffectFreeverb()
    : AudioEffectFreeverbBase(false) {
}

OSPEED
void AudioEffectFreeverb::update() {
    if (!begin()) {
        passThroughOnAllocationFailure();
        return;
    }

    audio_block_t *output = allocate();
    if (output == nullptr) {
        audio_block_t *input = receiveReadOnly();
        if (input != nullptr) {
            release(input);
        }
        return;
    }

    audio_block_t *input = receiveReadOnly();

    int32_t feedback;
    int32_t damping1;
    int32_t damping2;
    portENTER_CRITICAL(&parameter_mux);
    feedback = comb_feedback;
    damping1 = comb_damp1;
    damping2 = comb_damp2;
    portEXIT_CRITICAL(&parameter_mux);

    for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        const int16_t sample = input != nullptr ? input->data[i] : 0;
        output->data[i] = processSample(
            channels[0], sample, feedback, damping1, damping2);
    }

    transmit(output);
    release(output);
    if (input != nullptr) {
        release(input);
    }
}

AudioEffectFreeverbStereo::AudioEffectFreeverbStereo()
    : AudioEffectFreeverbBase(true) {
}

OSPEED
void AudioEffectFreeverbStereo::update() {
    if (!begin()) {
        passThroughOnAllocationFailure();
        return;
    }

    audio_block_t *output_left = allocate();
    audio_block_t *output_right = allocate();

    if (output_left == nullptr || output_right == nullptr) {
        if (output_left != nullptr) {
            release(output_left);
        }
        if (output_right != nullptr) {
            release(output_right);
        }

        audio_block_t *input = receiveReadOnly();
        if (input != nullptr) {
            release(input);
        }
        return;
    }

    audio_block_t *input = receiveReadOnly();

    int32_t feedback;
    int32_t damping1;
    int32_t damping2;
    portENTER_CRITICAL(&parameter_mux);
    feedback = comb_feedback;
    damping1 = comb_damp1;
    damping2 = comb_damp2;
    portEXIT_CRITICAL(&parameter_mux);

    for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        const int16_t sample = input != nullptr ? input->data[i] : 0;
        output_left->data[i] = processSample(
            channels[0], sample, feedback, damping1, damping2);
        output_right->data[i] = processSample(
            channels[1], sample, feedback, damping1, damping2);
    }

    transmit(output_left, 0);
    transmit(output_right, 1);
    release(output_left);
    release(output_right);
    if (input != nullptr) {
        release(input);
    }
}
