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
#include <string.h>

static portMUX_TYPE playback_state_mux = portMUX_INITIALIZER_UNLOCKED;


namespace {
static inline uint32_t sampleRateForFormat(uint8_t format) {
    switch (format) {
    case 0x81:
        return 44100;
    case 0x82:
        return 22050;
    case 0x83:
        return 11025;
    default:
        return 0;
    }
}

static inline int16_t sampleAt(const unsigned int *data, uint8_t format, uint32_t index) {
    switch (format) {
    case 0x81:
    case 0x82:
    case 0x83: {
        const uint32_t packed = data[index >> 1];
        return (index & 1U) ? (int16_t)(packed >> 16) : (int16_t)packed;
    }
    default:
        return 0;
    }
}
} // namespace

OSIZE
void AudioPlayMemory::play(const unsigned int *data)
{
    play(data, isLooping());
}

OSIZE
void AudioPlayMemory::play(const unsigned int *data, bool loop)
{
    if (data == nullptr) {
        stop();
        return;
    }

    const uint32_t formatWord = *data++;
    const float currentOutputSampleRate = sampleRate();
    portENTER_CRITICAL(&playback_state_mux);
    beginning = data;
    pcm_samples = nullptr;
    pcm_sample_rate = 0;
    raw_pcm = false;
    length = formatWord & 0xFFFFFFU;
    sample_index = 0;
    sample_fraction = 0.0f;
    current_playback_rate = target_playback_rate;
    output_sample_rate = currentOutputSampleRate;
    loop_enabled = loop;
    playing = (uint8_t)(formatWord >> 24);
    ++playback_generation;
    portEXIT_CRITICAL(&playback_state_mux);
}

OSIZE
bool AudioPlayMemory::play(const AudioSampleBuffer &buffer)
{
    return play(buffer, isLooping());
}

OSIZE
bool AudioPlayMemory::play(const AudioSampleBuffer &buffer, bool loop)
{
    const uint32_t rate = buffer.sampleRate();
    if (!buffer.isValid() ||
        (rate != 11025 && rate != 22050 && rate != 44100)) {
        return false;
    }

    const float currentOutputSampleRate = sampleRate();
    portENTER_CRITICAL(&playback_state_mux);
    beginning = nullptr;
    pcm_samples = buffer.samples();
    pcm_sample_rate = rate;
    raw_pcm = true;
    length = buffer.sampleCount();
    sample_index = 0;
    sample_fraction = 0.0f;
    current_playback_rate = target_playback_rate;
    output_sample_rate = currentOutputSampleRate;
    loop_enabled = loop;
    playing = 0x84; // signed 16-bit mono PCM supplied from AudioSampleBuffer
    ++playback_generation;
    portEXIT_CRITICAL(&playback_state_mux);
    return true;
}

OSIZE
void AudioPlayMemory::stop()
{
    portENTER_CRITICAL(&playback_state_mux);
    playing = 0;
    ++playback_generation;
    portEXIT_CRITICAL(&playback_state_mux);
}

OSIZE
bool AudioPlayMemory::isPlaying()
{
    portENTER_CRITICAL(&playback_state_mux);
    const bool result = playing != 0;
    portEXIT_CRITICAL(&playback_state_mux);
    return result;
}

OSIZE
void AudioPlayMemory::setLoop(bool enabled)
{
    portENTER_CRITICAL(&playback_state_mux);
    loop_enabled = enabled;
    portEXIT_CRITICAL(&playback_state_mux);
}

OSIZE
bool AudioPlayMemory::isLooping() const
{
    portENTER_CRITICAL(&playback_state_mux);
    const bool result = loop_enabled;
    portEXIT_CRITICAL(&playback_state_mux);
    return result;
}

OSIZE
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

OSIZE
float AudioPlayMemory::playbackRate() const
{
    portENTER_CRITICAL(&playback_state_mux);
    const float result = target_playback_rate;
    portEXIT_CRITICAL(&playback_state_mux);
    return result;
}

OSPEED
void AudioPlayMemory::update()
{
    portENTER_CRITICAL(&playback_state_mux);
    const uint8_t play_format = playing;
    const unsigned int *data = beginning;
    const int16_t *pcmData = pcm_samples;
    const uint32_t pcmRate = pcm_sample_rate;
    const bool rawPcm = raw_pcm;
    const bool loop = loop_enabled;
    const uint32_t total_samples = length;
    const uint32_t generation = playback_generation;
    uint32_t index = sample_index;
    float fraction = sample_fraction;
    float currentRate = current_playback_rate;
    const float targetRate = target_playback_rate;
    const float outputSampleRate = output_sample_rate;
    portEXIT_CRITICAL(&playback_state_mux);

    if (play_format == 0) {
        return;
    }

    audio_block_t *block = allocate();
    if (block == nullptr) {
        return;
    }

    const uint32_t sourceRate = rawPcm ? pcmRate : sampleRateForFormat(play_format);
    if ((rawPcm ? pcmData == nullptr : data == nullptr) ||
        total_samples == 0 || sourceRate == 0) {
        portENTER_CRITICAL(&playback_state_mux);
        if (playback_generation == generation) {
            playing = 0;
            ++playback_generation;
        }
        portEXIT_CRITICAL(&playback_state_mux);
        release(block);
        return;
    }

    // Defer loop restart until the next audio block.
    if (loop && index >= total_samples) {
        index = 0;
        fraction = 0.0f;
    }

    const float baseStep = (float)sourceRate / outputSampleRate;
    const bool unityRate = targetRate == 1.0f && currentRate == 1.0f;
    const bool directRate = unityRate && fraction == 0.0f &&
                            (float)sourceRate == outputSampleRate;
    int16_t *out = block->data;
    bool finished = false;

    if (directRate) {
        // Native-rate fast path: copy valid samples without per-sample
        // end-of-buffer checks, then zero-pad the remainder of the block.
        const uint32_t available = index < total_samples ? total_samples - index : 0;
        const uint32_t count = available < AUDIO_BLOCK_SAMPLES
                                   ? available
                                   : AUDIO_BLOCK_SAMPLES;
        for (uint32_t i = 0; i < count; ++i) {
            out[i] = rawPcm ? pcmData[index] : sampleAt(data, play_format, index);
            ++index;
        }
        if (count < AUDIO_BLOCK_SAMPLES) {
            memset(out + count, 0,
                   (AUDIO_BLOCK_SAMPLES - count) * sizeof(*out));
        }
    } else {
        uint32_t i = 0;
        for (; i < AUDIO_BLOCK_SAMPLES && index < total_samples; ++i) {
            const uint32_t index1 = (index + 1U < total_samples)
                                        ? index + 1U
                                        : index;
            const int32_t s0 = rawPcm ? pcmData[index] : sampleAt(data, play_format, index);
            const int32_t s1 = rawPcm ? pcmData[index1] : sampleAt(data, play_format, index1);
            out[i] = (int16_t)(s0 + (int32_t)((s1 - s0) * fraction));

            // Unity rate still needs sample-rate conversion for 11.025/22.05 kHz,
            // but it does not need the per-sample rate-smoothing calculation.
            if (unityRate) {
                fraction += baseStep;
            } else {
                const float rateDelta = targetRate - currentRate;
                if (rateDelta > -0.0001f && rateDelta < 0.0001f) {
                    currentRate = targetRate;
                } else {
                    currentRate += rateDelta * 0.0015f;
                }
                fraction += baseStep * currentRate;
            }
            const uint32_t advance = (uint32_t)fraction;
            index += advance;
            fraction -= (float)advance;

        }
        if (i < AUDIO_BLOCK_SAMPLES) {
            memset(out + i, 0, (AUDIO_BLOCK_SAMPLES - i) * sizeof(*out));
        }
    }

    if (!loop && index >= total_samples) {
        finished = true;
    }

    portENTER_CRITICAL(&playback_state_mux);
    if (playback_generation != generation) {
        portEXIT_CRITICAL(&playback_state_mux);
        release(block);
        return;
    }

    sample_index = index;
    sample_fraction = fraction;
    current_playback_rate = currentRate;
    if (finished) {
        playing = 0;
        ++playback_generation;
    }
    portEXIT_CRITICAL(&playback_state_mux);

    transmit(block);
    release(block);
}

OSIZE
uint32_t AudioPlayMemory::positionMillis()
{
    portENTER_CRITICAL(&playback_state_mux);
    const uint8_t format = playing;
    const uint32_t index = sample_index;
    const float fraction = sample_fraction;
    const bool rawPcm = raw_pcm;
    const uint32_t pcmRate = pcm_sample_rate;
    portEXIT_CRITICAL(&playback_state_mux);

    if (format == 0) {
        return 0;
    }

    const uint32_t sourceRate = rawPcm ? pcmRate : sampleRateForFormat(format);
    if (sourceRate == 0) {
        return 0;
    }

    return (uint32_t)(((double)index + fraction) * 1000.0 / (double)sourceRate);
}

OSIZE
uint32_t AudioPlayMemory::lengthMillis()
{
    portENTER_CRITICAL(&playback_state_mux);
    const uint8_t format = playing;
    const uint32_t samples = length;
    const bool rawPcm = raw_pcm;
    const uint32_t pcmRate = pcm_sample_rate;
    portEXIT_CRITICAL(&playback_state_mux);

    if (format == 0) {
        return 0;
    }

    const uint32_t sourceRate = rawPcm ? pcmRate : sampleRateForFormat(format);
    if (sourceRate == 0) {
        return 0;
    }

    return (uint32_t)(((uint64_t)samples * 1000ULL) / sourceRate);
}
