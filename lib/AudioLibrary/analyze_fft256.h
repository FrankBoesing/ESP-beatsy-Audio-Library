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

#ifndef analyze_fft256_h_
#define analyze_fft256_h_

#include <Arduino.h>
#include <stdint.h>
#include "AudioStream.h"

extern "C" {
extern const int16_t AudioWindowHanning256[];
extern const int16_t AudioWindowBartlett256[];
extern const int16_t AudioWindowBlackman256[];
extern const int16_t AudioWindowFlattop256[];
extern const int16_t AudioWindowBlackmanHarris256[];
extern const int16_t AudioWindowNuttall256[];
extern const int16_t AudioWindowBlackmanNuttall256[];
extern const int16_t AudioWindowWelch256[];
extern const int16_t AudioWindowHamming256[];
extern const int16_t AudioWindowCosine256[];
extern const int16_t AudioWindowTukey256[];
}

class AudioAnalyzeFFT256 : public AudioStream {
public:
    AudioAnalyzeFFT256();

    bool available();
    float read(unsigned int binNumber);
    float read(unsigned int binFirst, unsigned int binLast);
    void averageTogether(uint8_t n);
    void windowFunction(const int16_t *w);
    void update(void) override;

    uint16_t output[128] __attribute__((aligned(4)));

private:
    static constexpr uint16_t FFT_SIZE = 256;
    static constexpr uint16_t NUM_BINS = 128;

    void processFrame(void);

    const int16_t *window;
    int16_t frameSamples[FFT_SIZE];
    uint16_t samplesCollected;

    alignas(16) float fftBuffer[FFT_SIZE * 2];
    uint64_t accumulatedPower[NUM_BINS];
    uint8_t naverage;
    uint8_t averageCount;
    volatile bool outputflag;

    audio_block_t *inputQueueArray[1];
};

#endif // analyze_fft256_h_
