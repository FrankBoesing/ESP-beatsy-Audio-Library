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

#include "effect_granular.h"
#include <cmath>
#include "utility/dspinst.h"

namespace {
constexpr uint32_t FIXED_POINT_ONE = 1U << 16;
constexpr uint32_t MAX_FIXED_POINT_LENGTH = UINT32_MAX >> 16;
constexpr uint32_t MIN_GLITCH_LENGTH = 100;
constexpr uint32_t FADE_LENGTH = 20;
} // namespace

AudioEffectGranular::AudioEffectGranular() : AudioStream(1, inputQueueArray) {}

void AudioEffectGranular::begin(int16_t *sampleBank, uint32_t maxLength) {
    portENTER_CRITICAL(&_configMux);
    _sampleBank = sampleBank;
    _maxSampleLength =
        sampleBank == nullptr ? 0 : (maxLength < MAX_FIXED_POINT_LENGTH ? maxLength : MAX_FIXED_POINT_LENGTH);
    _freezeLength = 0;
    _glitchLength = 0;
    _requestedMode = Mode::OFF;
    ++_configurationGeneration;
    portEXIT_CRITICAL(&_configMux);
}

void AudioEffectGranular::setSpeed(float ratio) {
    if (!std::isfinite(ratio)) {
        ratio = 1.0f;
    } else if (ratio < 0.125f) {
        ratio = 0.125f;
    } else if (ratio > 8.0f) {
        ratio = 8.0f;
    }

    const uint32_t rate = (uint32_t)(ratio * FIXED_POINT_ONE + 0.5f);
    portENTER_CRITICAL(&_configMux);
    _playbackRate = rate;
    portEXIT_CRITICAL(&_configMux);
}

void AudioEffectGranular::beginFreeze(float grainLengthMs) {
    if (!std::isfinite(grainLengthMs) || grainLengthMs <= 0.0f) {
        return;
    }

    const float samples = grainLengthMs * (sampleRate() * 0.001f) + 0.5f;
    beginFreezeSamples(samples >= (float)(UINT32_MAX) ? UINT32_MAX : (uint32_t)(samples));
}

void AudioEffectGranular::beginPitchShift(float grainLengthMs) {
    if (!std::isfinite(grainLengthMs) || grainLengthMs <= 0.0f) {
        return;
    }

    const float samples = grainLengthMs * (sampleRate() * 0.001f) + 0.5f;
    beginPitchShiftSamples(samples >= (float)(UINT32_MAX) ? UINT32_MAX : (uint32_t)(samples));
}

void AudioEffectGranular::beginFreezeSamples(uint32_t grainSamples) {
    portENTER_CRITICAL(&_configMux);
    if (_sampleBank == nullptr || _maxSampleLength == 0) {
        portEXIT_CRITICAL(&_configMux);
        return;
    }

    if (grainSamples == 0) {
        grainSamples = 1;
    }
    if (grainSamples > _maxSampleLength) {
        grainSamples = _maxSampleLength;
    }

    _freezeLength = grainSamples;
    _requestedMode = Mode::FREEZE;
    ++_configurationGeneration;
    portEXIT_CRITICAL(&_configMux);
}

void AudioEffectGranular::beginPitchShiftSamples(uint32_t grainSamples) {
    portENTER_CRITICAL(&_configMux);
    const uint32_t maximum = _maxSampleLength > 1 ? (_maxSampleLength - 1) / 3 : 0;
    if (_sampleBank == nullptr || maximum == 0) {
        portEXIT_CRITICAL(&_configMux);
        return;
    }

    if (grainSamples < MIN_GLITCH_LENGTH) {
        grainSamples = maximum < MIN_GLITCH_LENGTH ? maximum : MIN_GLITCH_LENGTH;
    } else if (grainSamples > maximum) {
        grainSamples = maximum;
    }

    _glitchLength = grainSamples;
    _requestedMode = Mode::PITCH_SHIFT;
    ++_configurationGeneration;
    portEXIT_CRITICAL(&_configMux);
}

void AudioEffectGranular::stop() {
    portENTER_CRITICAL(&_configMux);
    _requestedMode = Mode::OFF;
    ++_configurationGeneration;
    portEXIT_CRITICAL(&_configMux);
}

void AudioEffectGranular::resetPlayback(Mode mode, uint32_t freezeLength, uint32_t glitchLength) {
    _activeMode = mode;
    _activeFreezeLength = freezeLength;
    _activeGlitchLength = glitchLength;
    _phase = 0;
    _writeHead = 0;
    _previousInput = 0;
    _capturing = false;
    _sampleLoaded = false;
    _sampleRequested = mode != Mode::OFF;
}

void AudioEffectGranular::update() {
    Mode requestedMode;
    uint32_t freezeLength;
    uint32_t glitchLength;
    uint32_t generation;
    uint32_t playbackRate;
    int16_t *sampleBank;
    uint32_t maxSampleLength;

    portENTER_CRITICAL(&_configMux);
    requestedMode = _requestedMode;
    freezeLength = _freezeLength;
    glitchLength = _glitchLength;
    generation = _configurationGeneration;
    playbackRate = _playbackRate;
    sampleBank = _sampleBank;
    maxSampleLength = _maxSampleLength;
    portEXIT_CRITICAL(&_configMux);

    audio_block_t *block = receiveWritable();
    if (block == nullptr) {
        return;
    }

    if (generation != _activeGeneration) {
        _activeGeneration = generation;
        resetPlayback(requestedMode, freezeLength, glitchLength);
    }

    if (sampleBank == nullptr || maxSampleLength == 0 || _activeMode == Mode::OFF) {
        _previousInput = block->data[AUDIO_BLOCK_SAMPLES - 1];
        transmit(block);
        release(block);
        return;
    }

    const uint32_t cycleLength = (_activeMode == Mode::FREEZE ? _activeFreezeLength : _activeGlitchLength) << 16;
    uint32_t phaseIncrement = playbackRate;
    while (phaseIncrement >= cycleLength) {
        phaseIncrement -= cycleLength;
    }

    for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        const int16_t input = block->data[i];
        const bool zeroCrossing = (input < 0 && _previousInput >= 0) || (input >= 0 && _previousInput < 0);

        if (_activeMode == Mode::FREEZE) {
            if (!_sampleLoaded) {
                if (_sampleRequested && zeroCrossing) {
                    _sampleRequested = false;
                    _capturing = true;
                    _writeHead = 0;
                }

                if (_capturing) {
                    sampleBank[_writeHead++] = input;
                    if (_writeHead == _activeFreezeLength) {
                        _capturing = false;
                        _sampleLoaded = true;
                        _phase = 0;
                    }
                }
            }

            if (_sampleLoaded) {
                block->data[i] = sampleBank[_phase >> 16];
                if (phaseIncrement != 0) {
                    if (_phase >= cycleLength - phaseIncrement) {
                        _phase -= cycleLength - phaseIncrement;
                    } else {
                        _phase += phaseIncrement;
                    }
                }
            }
        } else if (_activeMode == Mode::PITCH_SHIFT) {
            if (_sampleRequested && !_capturing && zeroCrossing) {
                _sampleRequested = false;
                _capturing = true;
                _writeHead = 0;
            }

            if (_capturing) {
                sampleBank[_writeHead++] = input;
                if (_writeHead == _activeGlitchLength) {
                    const uint32_t fadeLength = _activeGlitchLength < FADE_LENGTH ? _activeGlitchLength : FADE_LENGTH;
                    const uint32_t fadeStart = _activeGlitchLength - fadeLength;

                    for (uint32_t sample = 0; sample < _activeGlitchLength; ++sample) {
                        int16_t value = 0;
                        if (sample >= 2 && sample < fadeStart) {
                            value = sampleBank[sample];
                        } else if (sample >= 2) {
                            const uint32_t gain = ((_activeGlitchLength - sample) << 15) / fadeLength;
                            const int32_t scaled =
                                (int32_t)(sampleBank[sample]) * (int32_t)(gain);
                            value = (int16_t)(signed_saturate_rshift(scaled, 16, 15));
                        }
                        sampleBank[sample + _activeGlitchLength] = value;
                        sampleBank[sample + 2 * _activeGlitchLength] = value;
                    }

                    _capturing = false;
                    _sampleLoaded = true;
                    _sampleRequested = true;
                    _phase = 0;
                }
            }

            if (_sampleLoaded) {
                block->data[i] = sampleBank[2 * _activeGlitchLength + (_phase >> 16)];
                if (phaseIncrement != 0) {
                    if (_phase >= cycleLength - phaseIncrement) {
                        _phase -= cycleLength - phaseIncrement;
                    } else {
                        _phase += phaseIncrement;
                    }
                }
            }
        }

        _previousInput = input;
    }

    transmit(block);
    release(block);
}
