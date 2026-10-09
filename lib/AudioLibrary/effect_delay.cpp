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
#include <esp_heap_caps.h>
#include "effect_delay.h"

AudioEffectDelay::~AudioEffectDelay() { freeBuffer(); }

bool AudioEffectDelay::allocateBuffer() {
    if (buffer) return true;

    uint32_t n = (uint32_t)((float)maxDelayMs * (AUDIO_SAMPLE_RATE_EXACT / 1000.0f) + 0.5f);
    // one extra block so a full-length delay never reads data being written
    const uint32_t samples = n + AUDIO_BLOCK_SAMPLES;
    const size_t bytes = (size_t)samples * sizeof(int16_t);

    bool inPsram = true;
    int16_t *p = (int16_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        inPsram = false;
        p = (int16_t *)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!p) return false;

    memset(p, 0, bytes);
    capacity = samples;
    writeIndex = 0;
    psramBuffer = inPsram;
    buffer = p;
    return true;
}

void AudioEffectDelay::freeBuffer() {
    int16_t *p = buffer;
    activemask = 0;
    buffer = nullptr;
    capacity = 0;
    psramBuffer = false;
    if (p) heap_caps_free(p);
}

float AudioEffectDelay::maxDelay() const {
    if (!buffer) return 0.0f;
    return (float)(capacity - AUDIO_BLOCK_SAMPLES) * (1000.0f / AUDIO_SAMPLE_RATE_EXACT);
}

void AudioEffectDelay::delay(uint8_t channel, float milliseconds) {
    if (channel >= 8) return;
    if (milliseconds < 0.0f) milliseconds = 0.0f;
    if (!allocateBuffer()) return;

    uint32_t n = (uint32_t)(milliseconds * (AUDIO_SAMPLE_RATE_EXACT / 1000.0f) + 0.5f);
    const uint32_t nmax = capacity - AUDIO_BLOCK_SAMPLES;
    if (n > nmax) n = nmax;
    position[channel] = n;
    activemask |= (uint8_t)(1 << channel);
}

void AudioEffectDelay::disable(uint8_t channel) {
    if (channel >= 8) return;
    activemask &= (uint8_t)~(1 << channel);
}

void AudioEffectDelay::update(void) {
    int16_t *buf = buffer;
    if (!buf) {
        audio_block_t *in = receiveReadOnly();
        if (in) release(in);
        return;
    }

    // store the incoming block (silence if none arrived)
    const uint32_t start = writeIndex;
    audio_block_t *in = receiveReadOnly();
    uint32_t first = capacity - start;
    if (first > AUDIO_BLOCK_SAMPLES) first = AUDIO_BLOCK_SAMPLES;
    if (in) {
        memcpy(buf + start, in->data, first * sizeof(int16_t));
        if (first < AUDIO_BLOCK_SAMPLES) memcpy(buf, in->data + first, (AUDIO_BLOCK_SAMPLES - first) * sizeof(int16_t));
        release(in);
    } else {
        memset(buf + start, 0, first * sizeof(int16_t));
        if (first < AUDIO_BLOCK_SAMPLES) memset(buf, 0, (AUDIO_BLOCK_SAMPLES - first) * sizeof(int16_t));
    }
    uint32_t next = start + AUDIO_BLOCK_SAMPLES;
    if (next >= capacity) next -= capacity;
    writeIndex = next;

    // output = input delayed by position[channel] samples
    const uint8_t mask = activemask;
    for (uint8_t channel = 0; channel < 8; channel++) {
        if (!(mask & (1 << channel))) continue;
        audio_block_t *output = allocate();
        if (!output) continue;

        uint32_t n = position[channel];
        if (n > capacity - AUDIO_BLOCK_SAMPLES) n = capacity - AUDIO_BLOCK_SAMPLES;
        uint32_t rd = (start + capacity - n) % capacity;

        uint32_t len1 = capacity - rd;
        if (len1 > AUDIO_BLOCK_SAMPLES) len1 = AUDIO_BLOCK_SAMPLES;
        memcpy(output->data, buf + rd, len1 * sizeof(int16_t));
        if (len1 < AUDIO_BLOCK_SAMPLES)
            memcpy(output->data + len1, buf, (AUDIO_BLOCK_SAMPLES - len1) * sizeof(int16_t));

        transmit(output, channel);
        release(output);
    }
}
