/* Audio Library for Teensy 3.X
 * Copyright (c) 2014, Paul Stoffregen, paul@pjrc.com
 *
 * Development of this audio library was funded by PJRC.COM, LLC by sales of
 * Teensy and Audio Adaptor boards.  Please support PJRC's efforts to develop
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

#include <Arduino.h>
#include "play_memory.h"
#include "utility/dspinst.h"

extern "C" {
extern const int16_t ulaw_decode_table[256];
};

static portMUX_TYPE playback_state_mux = portMUX_INITIALIZER_UNLOCKED;


namespace {
static uint32_t sampleRateForFormat(uint8_t format) {
    switch (format) {
    case 0x01:
    case 0x81:
        return 44100;
    case 0x02:
    case 0x82:
        return 22050;
    case 0x03:
    case 0x83:
        return 11025;
    default:
        return 0;
    }
}

static int16_t sampleAt(const unsigned int *data, uint8_t format, uint32_t index) {
    switch (format) {
    case 0x81:
    case 0x82:
    case 0x83: {
        const uint32_t packed = data[index >> 1];
        return (index & 1U) ? (int16_t)(packed >> 16) : (int16_t)packed;
    }
    case 0x01:
    case 0x02:
    case 0x03: {
        const uint32_t packed = data[index >> 2];
        const uint8_t code = (uint8_t)(packed >> ((index & 3U) * 8U));
        return ulaw_decode_table[code];
    }
    default:
        return 0;
    }
}
} // namespace

void AudioPlayMemory::play(const unsigned int *data)
{
    if (data == nullptr) {
        stop();
        return;
    }

    const uint32_t formatWord = *data++;
    portENTER_CRITICAL(&playback_state_mux);
    beginning = data;
    length = formatWord & 0xFFFFFFU;
    sample_position = 0.0;
    current_playback_rate = target_playback_rate;
    playing = (uint8_t)(formatWord >> 24);
    ++playback_generation;
    portEXIT_CRITICAL(&playback_state_mux);
}

void AudioPlayMemory::stop()
{
    portENTER_CRITICAL(&playback_state_mux);
    playing = 0;
    ++playback_generation;
    portEXIT_CRITICAL(&playback_state_mux);
}

bool AudioPlayMemory::isPlaying()
{
    portENTER_CRITICAL(&playback_state_mux);
    const bool result = playing != 0;
    portEXIT_CRITICAL(&playback_state_mux);
    return result;
}

bool AudioPlayMemory::setPlaybackRate(float rate)
{
    if (!(rate >= 0.25f && rate <= 4.0f)) {
        return false;
    }

    portENTER_CRITICAL(&playback_state_mux);
    target_playback_rate = rate;
    portEXIT_CRITICAL(&playback_state_mux);
    return true;
}

float AudioPlayMemory::playbackRate() const
{
    portENTER_CRITICAL(&playback_state_mux);
    const float result = target_playback_rate;
    portEXIT_CRITICAL(&playback_state_mux);
    return result;
}

void AudioPlayMemory::update()
{
    portENTER_CRITICAL(&playback_state_mux);
    uint8_t play_format = playing;
    const unsigned int *data = beginning;
    const uint32_t total_samples = length;
    const uint32_t generation = playback_generation;
    double position = sample_position;
    float currentRate = current_playback_rate;
    const float targetRate = target_playback_rate;
    portEXIT_CRITICAL(&playback_state_mux);

    if (play_format == 0) {
        return;
    }

    audio_block_t *block = allocate();
    if (block == nullptr) {
        return;
    }

    const uint32_t sourceRate = sampleRateForFormat(play_format);
    if (data == nullptr || total_samples == 0 || sourceRate == 0) {
        portENTER_CRITICAL(&playback_state_mux);
        if (playback_generation == generation) {
            playing = 0;
            ++playback_generation;
        }
        portEXIT_CRITICAL(&playback_state_mux);
        release(block);
        return;
    }

    const double baseStep = (double)sourceRate / (double)AUDIO_SAMPLE_RATE_EXACT;
    int16_t *out = block->data;
    bool finished = false;

    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        if (position >= (double)total_samples) {
            out[i] = 0;
            finished = true;
            continue;
        }

        const uint32_t index0 = (uint32_t)position;
        const uint32_t index1 = (index0 + 1U < total_samples) ? index0 + 1U : index0;
        const float fraction = (float)(position - (double)index0);
        const int32_t s0 = sampleAt(data, play_format, index0);
        const int32_t s1 = sampleAt(data, play_format, index1);
        out[i] = (int16_t)(s0 + (int32_t)((s1 - s0) * fraction));

        // Smooth rate changes to avoid abrupt pitch modulation.
        currentRate += (targetRate - currentRate) * 0.0015f;
        position += baseStep * (double)currentRate;
    }

    if (position >= (double)total_samples) {
        finished = true;
    }

    portENTER_CRITICAL(&playback_state_mux);
    if (playback_generation != generation) {
        portEXIT_CRITICAL(&playback_state_mux);
        release(block);
        return;
    }

    sample_position = position;
    current_playback_rate = currentRate;
    if (finished) {
        playing = 0;
        ++playback_generation;
    }
    portEXIT_CRITICAL(&playback_state_mux);

    transmit(block);
    release(block);
}

uint32_t AudioPlayMemory::positionMillis()
{
    portENTER_CRITICAL(&playback_state_mux);
    const uint8_t format = playing;
    const double position = sample_position;
    portEXIT_CRITICAL(&playback_state_mux);

    if (format == 0) {
        return 0;
    }

    const uint32_t sourceRate = sampleRateForFormat(format);
    if (sourceRate == 0) {
        return 0;
    }

    return (uint32_t)(position * 1000.0 / (double)sourceRate);
}

uint32_t AudioPlayMemory::lengthMillis()
{
    portENTER_CRITICAL(&playback_state_mux);
    const uint8_t format = playing;
    const uint32_t samples = length;
    portEXIT_CRITICAL(&playback_state_mux);

    if (format == 0) {
        return 0;
    }

    const uint32_t sourceRate = sampleRateForFormat(format);
    if (sourceRate == 0) {
        return 0;
    }

    return (uint32_t)(((uint64_t)samples * 1000ULL) / sourceRate);
}
