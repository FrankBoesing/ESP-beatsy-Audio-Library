/* Audio Library for Teensy 3.X
 * Copyright (c) 2014, Paul Stoffregen, paul@pjrc.com
 *
 * Development of this audio library was funded by PJRC.COM, LLC by sales of
 * Teensy and Audio Adaptor boards. Please support PJRC's efforts to develop
 * open source software by purchasing other PJRC or Teensy products.
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
#include <stdint.h>
#include <string.h>

#include "analyze_fft1024.h"
#include "utility/audio_fft_backend.h"

AudioAnalyzeFFT1024::AudioAnalyzeFFT1024()
    : AudioStream(1, inputQueueArray),
      window(AudioWindowHanning1024),
      blocksCollected(0),
      outputflag(false) {
    memset(blocklist, 0, sizeof(blocklist));
    memset(fftBuffer, 0, sizeof(fftBuffer));
    memset(output, 0, sizeof(output));
    AudioFFT::initialize();
}

AudioAnalyzeFFT1024::~AudioAnalyzeFFT1024() {
    // Release retained AudioMemory blocks if this analyzer is destroyed before
    // a complete frame arrives. This is also safe with the recursive update
    // mutex used by AudioStream's optional dynamic-lifetime support.
    AudioStream::disableUpdates();
    for (uint16_t i = 0; i < blocksCollected; ++i) {
        if (blocklist[i] != nullptr) {
            release(blocklist[i]);
            blocklist[i] = nullptr;
        }
    }
    blocksCollected = 0;
    AudioStream::enableUpdates();
}

bool AudioAnalyzeFFT1024::available() {
    AudioStream::disableUpdates();
    const bool result = outputflag;
    outputflag = false;
    AudioStream::enableUpdates();
    return result;
}

float AudioAnalyzeFFT1024::read(unsigned int binNumber) {
    if (binNumber >= NUM_BINS) {
        return 0.0f;
    }
    return static_cast<float>(output[binNumber]) * (1.0f / 16384.0f);
}

float AudioAnalyzeFFT1024::read(unsigned int binFirst, unsigned int binLast) {
    if (binFirst > binLast) {
        const unsigned int tmp = binLast;
        binLast = binFirst;
        binFirst = tmp;
    }
    if (binFirst >= NUM_BINS) {
        return 0.0f;
    }
    if (binLast >= NUM_BINS) {
        binLast = NUM_BINS - 1;
    }

    uint32_t sum = 0;
    for (unsigned int bin = binFirst; bin <= binLast; ++bin) {
        sum += output[bin];
    }
    return static_cast<float>(sum) * (1.0f / 16384.0f);
}

void AudioAnalyzeFFT1024::averageTogether(uint8_t n) {
    // Kept for source compatibility with the Teensy API; averaging remains
    // intentionally unsupported by the original FFT1024 analyzer.
    (void)n;
}

void AudioAnalyzeFFT1024::windowFunction(const int16_t *w) {
    AudioStream::disableUpdates();
    window = w;
    AudioStream::enableUpdates();
}

void AudioAnalyzeFFT1024::processFrame(void) {
    constexpr float q15Scale = 1.0f / 32768.0f;

    // Read directly from retained blocks. This deliberately keeps the input
    // blocks allocated until a complete frame is available; AudioMemory()
    // must be sized to include these blocks plus all other graph requirements.
    uint16_t sampleIndex = 0;
    for (uint16_t blockIndex = 0; blockIndex < BLOCKS_PER_FRAME; ++blockIndex) {
        const int16_t *samples = blocklist[blockIndex]->data;
        for (uint16_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i, ++sampleIndex) {
            float sample = static_cast<float>(samples[i]) * q15Scale;
            if (window != nullptr) {
                sample *= static_cast<float>(window[sampleIndex]) * q15Scale;
            }
            fftBuffer[2U * sampleIndex] = sample;
            fftBuffer[2U * sampleIndex + 1U] = 0.0f;
        }
    }

    if (!AudioFFT::transform(fftBuffer, FFT_SIZE)) {
        return;
    }

    // Keep the output convention of the original analyzer.
    constexpr float outputScale = 32768.0f / FFT_SIZE;
    constexpr float powerScale = outputScale * outputScale;

    for (uint16_t bin = 0; bin < NUM_BINS; ++bin) {
        const uint32_t power =
            AudioFFT::magnitudeSquaredScaled(fftBuffer, bin, powerScale);
        output[bin] = AudioFFT::magnitudeFromSquared(power);
    }

    outputflag = true;
}

void AudioAnalyzeFFT1024::update(void) {
    audio_block_t *block = receiveReadOnly();
    if (block == nullptr) {
        return;
    }

    // Keep every input block until a complete 1024-sample frame is ready.
    // For 128-sample blocks this pins 8 blocks; for 64-sample blocks it pins
    // 16. The analyzer deliberately consumes AudioMemory pool capacity.
    blocklist[blocksCollected++] = block;

    if (blocksCollected < BLOCKS_PER_FRAME) {
        return;
    }

    processFrame();

    // 50% overlap: release the older half, retain the newer half and append
    // future input blocks after it for the next FFT frame.
    for (uint16_t i = 0; i < HALF_FRAME_BLOCKS; ++i) {
        release(blocklist[i]);
    }

    for (uint16_t i = 0; i < HALF_FRAME_BLOCKS; ++i) {
        blocklist[i] = blocklist[i + HALF_FRAME_BLOCKS];
    }
    for (uint16_t i = HALF_FRAME_BLOCKS; i < BLOCKS_PER_FRAME; ++i) {
        blocklist[i] = nullptr;
    }

    blocksCollected = HALF_FRAME_BLOCKS;
}
