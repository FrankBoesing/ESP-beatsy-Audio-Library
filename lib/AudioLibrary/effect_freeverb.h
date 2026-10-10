/*
 * Freeverb for the Teensy Audio Library, adapted for ESP32.
 * Copyright (c) 2018, Paul Stoffregen, paul@pjrc.com
 *
 * Development of this audio library was funded by PJRC.COM, LLC by sales of
 * Teensy and Audio Adaptor boards. Please support PJRC's efforts to develop
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

#ifndef effect_freeverb_h_
#define effect_freeverb_h_

#include "AudioStream.h"
#include <stddef.h>
#include <stdint.h>

// Shared implementation for the mono and stereo Freeverb effects.
// The delay lines are allocated as one contiguous block at begin().
class AudioEffectFreeverbBase : public AudioStream {
  public:
    ~AudioEffectFreeverbBase() override;

    // Allocate and clear delay-line storage. Call from setup() before starting
    // audio updates. update() retries lazily for compatibility with sketches
    // that do not call begin().
    bool begin();

    // True when delay-line storage had to be placed in PSRAM.
    bool usingPSRAM() const { return using_psram; }

    void roomsize(float n);
    void damping(float n);

  protected:
    explicit AudioEffectFreeverbBase(bool stereo);

    struct ChannelState {
        int16_t *comb[8] = {};
        uint16_t comb_length[8] = {};
        uint16_t comb_index[8] = {};
        int16_t comb_filter[8] = {};
        int16_t *allpass[4] = {};
        uint16_t allpass_length[4] = {};
        uint16_t allpass_index[4] = {};
    };

    int16_t processSample(ChannelState &channel, int16_t input,
                          int32_t feedback, int32_t damping1,
                          int32_t damping2);
    void passThroughOnAllocationFailure();

    audio_block_t *inputQueueArray[1] = {};

  private:
    void configureChannel(ChannelState &channel, size_t &offset,
                          const uint16_t *comb_lengths,
                          const uint16_t *allpass_lengths);

    ChannelState channels[2] = {};
    int16_t *delay_memory = nullptr;
    size_t delay_samples = 0;
    bool stereo_mode = false;
    bool using_psram = false;
    bool allocation_error_logged = false;

    int32_t comb_feedback = 27524;
    int32_t comb_damp1 = 6553;
    int32_t comb_damp2 = 26215;

    // Protect parameter snapshots against changes from another task/core.
    portMUX_TYPE parameter_mux = portMUX_INITIALIZER_UNLOCKED;
};

class AudioEffectFreeverb : public AudioEffectFreeverbBase {
  public:
    AudioEffectFreeverb();
    void update() override;
};

class AudioEffectFreeverbStereo : public AudioEffectFreeverbBase {
  public:
    AudioEffectFreeverbStereo();
    void update() override;
};

#endif
