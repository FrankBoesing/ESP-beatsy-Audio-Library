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

#ifndef play_memory_h_
#define play_memory_h_

#include <Arduino.h>
#include <AudioStream.h>
#include "AudioSampleBuffer.h"

class AudioPlayMemory : public AudioStream
{
public:
    AudioPlayMemory() : AudioStream(0, nullptr) {}

    void play(const unsigned int *data);
    void play(const unsigned int *data, bool loop);
    // Plays mono PCM samples owned by AudioSampleBuffer without copying them.
    // The buffer must outlive playback. Supported sample rates: 11025, 22050, 44100 Hz.
    bool play(const AudioSampleBuffer &buffer);
    bool play(const AudioSampleBuffer &buffer, bool loop);
    void stop();
    bool isPlaying();

    void setLoop(bool enabled);
    bool isLooping() const;

    // Playback-rate multiplier: 1.0 = original speed/pitch.
    // Resampling changes pitch too; this is not pitch-preserving time-stretch.
    // Accepted range: 0.25 ... 4.0. Returns false for invalid values.
    bool setPlaybackRate(float rate);
    float playbackRate() const;

    uint32_t positionMillis();
    uint32_t lengthMillis();
    void update() override;

private:
    const unsigned int *beginning = nullptr; // Legacy packed PCM sample data
    const int16_t *pcm_samples = nullptr;    // Non-owning AudioSampleBuffer data
    uint32_t length = 0;                    // Source samples, not output samples
    uint32_t pcm_sample_rate = 0;
    uint32_t playback_generation = 0;
    uint32_t sample_index = 0;
    float sample_fraction = 0.0f;           // Fractional source-sample position
    float current_playback_rate = 1.0f;     // Audio-update task only
    float target_playback_rate = 1.0f;      // Protected by playback_state_mux
    float output_sample_rate = AUDIO_SAMPLE_RATE_EXACT; // Captured at play() start
    bool raw_pcm = false;
    bool loop_enabled = false;              // Protected by playback_state_mux
    volatile uint8_t playing = 0;           // Encoded format or 0x84 for raw PCM
};

#endif
