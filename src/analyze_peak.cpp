/* Audio Library for Teensy 3.X
 * Copyright (c) 2014, Paul Stoffregen, paul@pjrc.com
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
 * notice shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <Arduino.h>
#include "analyze_peak.h"

bool AudioAnalyzePeak::available() {
    portENTER_CRITICAL(&_peakMux);
    const bool result = new_output;
    new_output = false;
    portEXIT_CRITICAL(&_peakMux);
    return result;
}

bool AudioAnalyzePeak::takeSamples(int16_t &minimum, int16_t &maximum) {
    portENTER_CRITICAL(&_peakMux);
    const bool result = have_samples;
    minimum = min_sample;
    maximum = max_sample;
    min_sample = 32767;
    max_sample = -32768;
    have_samples = false;
    portEXIT_CRITICAL(&_peakMux);
    return result;
}

float AudioAnalyzePeak::read() {
    int16_t minimum;
    int16_t maximum;
    if (!takeSamples(minimum, maximum)) {
        return 0.0f;
    }

    const int32_t minimumMagnitude = minimum < 0 ? -(int32_t)(minimum) : minimum;
    const int32_t maximumMagnitude = maximum < 0 ? -(int32_t)(maximum) : maximum;
    const int32_t peak = minimumMagnitude > maximumMagnitude ? minimumMagnitude : maximumMagnitude;
    return (float)(peak) / 32767.0f;
}

float AudioAnalyzePeak::readPeakToPeak() {
    int16_t minimum;
    int16_t maximum;
    if (!takeSamples(minimum, maximum)) {
        return 0.0f;
    }

    return (float)((int32_t)(maximum) - minimum) / 32767.0f;
}

OSPEED
void AudioAnalyzePeak::update() {
    audio_block_t *block = receiveReadOnly();
    if (block == nullptr) {
        return;
    }

    int16_t minimum = 32767;
    int16_t maximum = -32768;
    const int16_t *samples = block->data;

    for (int index = 0; index < AUDIO_BLOCK_SAMPLES; ++index) {
        const int16_t sample = samples[index];
        if (sample < minimum) {
            minimum = sample;
        }
        if (sample > maximum) {
            maximum = sample;
        }
    }

    portENTER_CRITICAL(&_peakMux);
    if (!have_samples) {
        min_sample = minimum;
        max_sample = maximum;
        have_samples = true;
    } else {
        if (minimum < min_sample) {
            min_sample = minimum;
        }
        if (maximum > max_sample) {
            max_sample = maximum;
        }
    }
    new_output = true;
    portEXIT_CRITICAL(&_peakMux);

    release(block);
}
