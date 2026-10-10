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

#ifndef analyze_fft1024_h_
#define analyze_fft1024_h_

#include <Arduino.h>
#include <stdint.h>
#include "AudioStream.h"

extern "C" {
extern const int16_t AudioWindowHanning1024[];
extern const int16_t AudioWindowBartlett1024[];
extern const int16_t AudioWindowBlackman1024[];
extern const int16_t AudioWindowFlattop1024[];
extern const int16_t AudioWindowBlackmanHarris1024[];
extern const int16_t AudioWindowNuttall1024[];
extern const int16_t AudioWindowBlackmanNuttall1024[];
extern const int16_t AudioWindowWelch1024[];
extern const int16_t AudioWindowHamming1024[];
extern const int16_t AudioWindowCosine1024[];
extern const int16_t AudioWindowTukey1024[];
}

class AudioAnalyzeFFT1024 : public AudioStream {
public:
    AudioAnalyzeFFT1024();
    ~AudioAnalyzeFFT1024() override;

    bool available();
    float read(unsigned int binNumber);
    float read(unsigned int binFirst, unsigned int binLast);
    void averageTogether(uint8_t n);
    void windowFunction(const int16_t *w);
    void update(void) override;

    uint16_t output[512] __attribute__((aligned(4)));

private:
    static constexpr uint16_t FFT_SIZE = 1024;
    static constexpr uint16_t NUM_BINS = 512;
    static constexpr uint16_t BLOCKS_PER_FRAME = FFT_SIZE / AUDIO_BLOCK_SAMPLES;
    static constexpr uint16_t HALF_FRAME_BLOCKS = BLOCKS_PER_FRAME / 2;

    static_assert((FFT_SIZE % AUDIO_BLOCK_SAMPLES) == 0,
                  "FFT1024 requires AUDIO_BLOCK_SAMPLES to divide 1024");
    static_assert((BLOCKS_PER_FRAME % 2) == 0,
                  "FFT1024 requires an even number of audio blocks for 50% overlap");

    void processFrame(void);

    const int16_t *window;
    // Keep input blocks until a full frame is ready. After processing, release
    // the first half and retain the second half for the next 50%-overlapped FFT.
    audio_block_t *blocklist[BLOCKS_PER_FRAME];
    uint16_t blocksCollected;

    alignas(16) float fftBuffer[FFT_SIZE * 2];
    volatile bool outputflag;

    audio_block_t *inputQueueArray[1];
};

#endif // analyze_fft1024_h_
