/*
 * Copyright (c) 2018 John-Michael Reed
 * bleeplabs.com
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef effect_granular_h_
#define effect_granular_h_

#include "AudioStream.h"

class AudioEffectGranular : public AudioStream {
  public:
    AudioEffectGranular();

    // The caller owns the buffer; it must remain valid while the effect uses it.
    void begin(int16_t *sampleBank, uint32_t maxLength);
    void setSpeed(float ratio);
    void beginFreeze(float grainLengthMs);
    void beginPitchShift(float grainLengthMs);
    void stop();

    void update() override;

  private:
    enum class Mode : uint8_t {
        OFF,
        FREEZE,
        PITCH_SHIFT,
    };

    void beginFreezeSamples(uint32_t grainSamples);
    void beginPitchShiftSamples(uint32_t grainSamples);
    void resetPlayback(Mode mode, uint32_t freezeLength, uint32_t glitchLength);

    audio_block_t *inputQueueArray[1] = {};

    portMUX_TYPE _configMux = portMUX_INITIALIZER_UNLOCKED;
    int16_t *_sampleBank = nullptr;
    uint32_t _maxSampleLength = 0;
    uint32_t _playbackRate = 1U << 16;
    uint32_t _freezeLength = 0;
    uint32_t _glitchLength = 0;
    uint32_t _configurationGeneration = 0;
    Mode _requestedMode = Mode::OFF;

    uint32_t _activeGeneration = 0;
    Mode _activeMode = Mode::OFF;
    uint32_t _activeFreezeLength = 0;
    uint32_t _activeGlitchLength = 0;
    uint32_t _phase = 0;
    uint32_t _writeHead = 0;
    int16_t _previousInput = 0;
    bool _capturing = false;
    bool _sampleLoaded = false;
    bool _sampleRequested = false;
};

#endif
