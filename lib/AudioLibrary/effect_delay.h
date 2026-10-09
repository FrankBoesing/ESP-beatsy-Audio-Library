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

/*
 * ESP32 port: the Teensy block queue (which pins blocks of the shared audio
 * pool) is replaced by a sample ring buffer. The buffer is allocated lazily on
 * the first delay() call, preferably in PSRAM, with internal RAM as fallback.
 */

#pragma once
#ifndef effect_delay_h_
#define effect_delay_h_

#include <Arduino.h>
#include "AudioStream.h"
#include "utility/dspinst.h"

// Default maximum delay time in milliseconds (same as Teensy 4.x).
#ifndef AUDIO_EFFECT_DELAY_MAX_MS
#define AUDIO_EFFECT_DELAY_MAX_MS 4000
#endif

class AudioEffectDelay : public AudioStream {
  public:
    explicit AudioEffectDelay(uint32_t maxDelayMs = AUDIO_EFFECT_DELAY_MAX_MS)
        : AudioStream(1, inputQueueArray), maxDelayMs(maxDelayMs) {
        inputQueueArray[0] = nullptr;
        for (uint8_t i = 0; i < 8; i++) position[i] = 0;
    }
    ~AudioEffectDelay() override;

    AudioEffectDelay(const AudioEffectDelay &) = delete;
    AudioEffectDelay &operator=(const AudioEffectDelay &) = delete;

    // Sets the delay of an output channel (0..7). The buffer is allocated on
    // first use; if no memory is available the channel stays disabled.
    void delay(uint8_t channel, float milliseconds);
    void disable(uint8_t channel);

    // Maximum delay in ms that can actually be used (0 before first delay()).
    float maxDelay() const;
    bool usingPSRAM() const { return psramBuffer; }

    virtual void update(void);

  private:
    bool allocateBuffer();
    void freeBuffer();

    uint32_t maxDelayMs;
    int16_t *volatile buffer = nullptr;
    uint32_t capacity = 0;   // samples in ring buffer
    uint32_t writeIndex = 0; // next sample to be written
    bool psramBuffer = false;
    volatile uint8_t activemask = 0;
    uint32_t position[8]; // delay in samples per channel
    audio_block_t *inputQueueArray[1];
};

#endif
