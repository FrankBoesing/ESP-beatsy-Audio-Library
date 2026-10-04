#include "synth_dc.h"

#include <cmath>
#include <limits>

#include "utility/dspinst.h"

namespace {
constexpr int32_t DC_LEVEL_SCALE = 2147418112;
}

AudioSynthWaveformDc::AudioSynthWaveformDc() : AudioStream(0, nullptr) {}

int32_t AudioSynthWaveformDc::levelToFixedPoint(float level) {
    if (!std::isfinite(level)) {
        level = 0.0f;
    } else if (level > 1.0f) {
        level = 1.0f;
    } else if (level < -1.0f) {
        level = -1.0f;
    }

    return static_cast<int32_t>(level * static_cast<float>(DC_LEVEL_SCALE));
}

void AudioSynthWaveformDc::amplitude(float level) {
    const int32_t target = levelToFixedPoint(level);

    portENTER_CRITICAL(&_configMux);
    _magnitude = target;
    _target = target;
    _increment = 0;
    ++_configurationGeneration;
    portEXIT_CRITICAL(&_configMux);
}

void AudioSynthWaveformDc::amplitude(float level, float milliseconds) {
    if (!std::isfinite(milliseconds) || milliseconds <= 0.0f) {
        amplitude(level);
        return;
    }

    const float transitionSamples = milliseconds * (sampleRate() / 1000.0f);
    const uint32_t sampleCount = !std::isfinite(transitionSamples) ||
                                         transitionSamples >= static_cast<float>(std::numeric_limits<uint32_t>::max())
                                     ? std::numeric_limits<uint32_t>::max()
                                     : static_cast<uint32_t>(transitionSamples);
    if (sampleCount == 0) {
        amplitude(level);
        return;
    }

    const int32_t target = levelToFixedPoint(level);
    portENTER_CRITICAL(&_configMux);
    const int64_t difference = static_cast<int64_t>(target) - _magnitude;

    if (difference == 0) {
        _target = target;
        _increment = 0;
    } else {
        int64_t increment = difference / sampleCount;
        if (increment == 0) {
            increment = difference > 0 ? 1 : -1;
        }
        _target = target;
        _increment = static_cast<int32_t>(increment);
    }

    ++_configurationGeneration;
    portEXIT_CRITICAL(&_configMux);
}

float AudioSynthWaveformDc::read() const {
    portENTER_CRITICAL(&_configMux);
    const int32_t magnitude = _magnitude;
    portEXIT_CRITICAL(&_configMux);

    return static_cast<float>(magnitude) / static_cast<float>(DC_LEVEL_SCALE);
}

OSPEED
void AudioSynthWaveformDc::update() {
    int32_t magnitude;
    int32_t target;
    int32_t increment;
    uint32_t generation;

    portENTER_CRITICAL(&_configMux);
    magnitude = _magnitude;
    target = _target;
    increment = _increment;
    generation = _configurationGeneration;
    portEXIT_CRITICAL(&_configMux);

    audio_block_t *block = allocate();
    if (block == nullptr) {
        return;
    }

    if (generation != _activeGeneration) {
        _activeGeneration = generation;
        _activeMagnitude = magnitude;
        _activeTarget = target;
        _activeIncrement = increment;
        _transitioning = increment != 0;
    }

    int32_t current = _activeMagnitude;
    bool transitioning = _transitioning;

    for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        if (transitioning) {
            const int64_t next = static_cast<int64_t>(current) + _activeIncrement;
            if ((_activeIncrement > 0 && next >= _activeTarget) || (_activeIncrement < 0 && next <= _activeTarget)) {
                current = _activeTarget;
                transitioning = false;
            } else {
                current = static_cast<int32_t>(next);
            }
        }

        block->data[i] = static_cast<int16_t>(signed_saturate_rshift(current, 16, 16));
    }

    _activeMagnitude = current;
    _transitioning = transitioning;

    portENTER_CRITICAL(&_configMux);
    if (_configurationGeneration == generation) {
        _magnitude = current;
        if (!transitioning) {
            _target = current;
            _increment = 0;
        }
    }
    portEXIT_CRITICAL(&_configMux);

    transmit(block);
    release(block);
}
