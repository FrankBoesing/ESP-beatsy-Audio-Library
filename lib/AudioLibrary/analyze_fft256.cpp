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

#include "analyze_fft256.h"
#include "utility/audio_fft_backend.h"

AudioAnalyzeFFT256::AudioAnalyzeFFT256()
    : AudioStream(1, inputQueueArray),
      window(AudioWindowHanning256),
      samplesCollected(0),
      naverage(8),
      averageCount(0),
      outputflag(false) {
    memset(frameSamples, 0, sizeof(frameSamples));
    memset(fftBuffer, 0, sizeof(fftBuffer));
    memset(accumulatedPower, 0, sizeof(accumulatedPower));
    memset(output, 0, sizeof(output));
    AudioFFT::initialize();
}

bool AudioAnalyzeFFT256::available() {
    AudioStream::disableUpdates();
    const bool result = outputflag;
    outputflag = false;
    AudioStream::enableUpdates();
    return result;
}

float AudioAnalyzeFFT256::read(unsigned int binNumber) {
    if (binNumber >= NUM_BINS) {
        return 0.0f;
    }
    return static_cast<float>(output[binNumber]) * (1.0f / 16384.0f);
}

float AudioAnalyzeFFT256::read(unsigned int binFirst, unsigned int binLast) {
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

void AudioAnalyzeFFT256::averageTogether(uint8_t n) {
    if (n == 0) {
        n = 1;
    }

    AudioStream::disableUpdates();
    if (naverage != n) {
        naverage = n;
        averageCount = 0;
        memset(accumulatedPower, 0, sizeof(accumulatedPower));
    }
    AudioStream::enableUpdates();
}

void AudioAnalyzeFFT256::windowFunction(const int16_t *w) {
    AudioStream::disableUpdates();
    window = w;
    AudioStream::enableUpdates();
}

void AudioAnalyzeFFT256::processFrame(void) {
    constexpr float q15Scale = 1.0f / 32768.0f;

    // Load real input in interleaved complex form; the imaginary components
    // are zero. Window coefficients are Q15, as in the Teensy API.
    for (uint16_t i = 0; i < FFT_SIZE; ++i) {
        float sample = static_cast<float>(frameSamples[i]) * q15Scale;
        if (window != nullptr) {
            sample *= static_cast<float>(window[i]) * q15Scale;
        }
        fftBuffer[2U * i] = sample;
        fftBuffer[2U * i + 1U] = 0.0f;
    }

    if (!AudioFFT::transform(fftBuffer, FFT_SIZE)) {
        return;
    }

    // Legacy output scaling. For an unwindowed full-scale sine, the positive
    // fundamental bin is approximately 16384; read() divides by that value.
    constexpr float outputScale = 32768.0f / FFT_SIZE;
    constexpr float powerScale = outputScale * outputScale;

    // As recommended for spectral averaging, average magnitude squared and
    // take the square root only after the averaging interval has completed.
    for (uint16_t bin = 0; bin < NUM_BINS; ++bin) {
        accumulatedPower[bin] +=
            AudioFFT::magnitudeSquaredScaled(fftBuffer, bin, powerScale);
    }

    ++averageCount;
    if (averageCount < naverage) {
        return;
    }

    for (uint16_t bin = 0; bin < NUM_BINS; ++bin) {
        const uint64_t meanPower = accumulatedPower[bin] / averageCount;
        const uint32_t boundedPower =
            (meanPower > UINT32_MAX) ? UINT32_MAX :
            static_cast<uint32_t>(meanPower);

        output[bin] = AudioFFT::magnitudeFromSquared(boundedPower);
        accumulatedPower[bin] = 0;
    }

    averageCount = 0;
    outputflag = true;
}

void AudioAnalyzeFFT256::update(void) {
    audio_block_t *block = receiveReadOnly();
    if (block == nullptr) {
        return;
    }

    // Build frames from the incoming stream. Each new frame overlaps the
    // previous frame by 50%, independent of AUDIO_BLOCK_SAMPLES (64/128/etc.).
    for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        frameSamples[samplesCollected++] = block->data[i];

        if (samplesCollected == FFT_SIZE) {
            processFrame();
            memmove(frameSamples, frameSamples + (FFT_SIZE / 2),
                    (FFT_SIZE / 2) * sizeof(frameSamples[0]));
            samplesCollected = FFT_SIZE / 2;
        }
    }

    release(block);
}
