#include "filter_fir.h"

#include <cstring>

namespace {
int16_t q15ToPcm(int64_t value) {
    constexpr int64_t HALF = int64_t{1} << 14;

    if (value >= 0) {
        value = (value + HALF) >> 15;
    } else {
        value = -(((-value) + HALF) >> 15);
    }

    if (value > 32767) {
        return 32767;
    }

    if (value < -32768) {
        return -32768;
    }

    return static_cast<int16_t>(value);
}
} // namespace

AudioFilterFIR::AudioFilterFIR() : AudioStream(1, inputQueueArray) {}

bool AudioFilterFIR::validCoefficientCount(int coefficientCount) {
    return coefficientCount >= 4 && coefficientCount <= FIR_MAX_COEFFS &&
           (coefficientCount & 1) == 0;
}

void AudioFilterFIR::begin(const short *coefficients, int coefficientCount) {
    portENTER_CRITICAL(&_configMux);

    _mode = Mode::OFF;
    _coefficientCount = 0;
    _resetHistory = true;
    ++_configurationGeneration;

    if (coefficients == FIR_PASSTHRU) {
        _mode = Mode::BYPASS;
    } else
    if (coefficients != nullptr && validCoefficientCount(coefficientCount)) {
        std::memcpy(_coefficients, coefficients, static_cast<size_t>(coefficientCount) * sizeof(short));
        _coefficientCount = static_cast<size_t>(coefficientCount);
        _mode = Mode::ACTIVE;
    }

    portEXIT_CRITICAL(&_configMux);
}

void AudioFilterFIR::end() {
    portENTER_CRITICAL(&_configMux);
    _mode = Mode::OFF;
    _coefficientCount = 0;
    _resetHistory = true;
    ++_configurationGeneration;
    portEXIT_CRITICAL(&_configMux);
}

void AudioFilterFIR::update() {
    Mode mode;
    size_t coefficientCount;
    bool resetHistory;
    uint32_t generation;
    short coefficients[FIR_MAX_COEFFS];

    portENTER_CRITICAL(&_configMux);
    mode = _mode;
    generation = _configurationGeneration;
    portEXIT_CRITICAL(&_configMux);

    audio_block_t *block = mode == Mode::ACTIVE ? receiveWritable() : receiveReadOnly();
    if (block == nullptr) {
        return;
    }

    portENTER_CRITICAL(&_configMux);
    if (generation != _configurationGeneration) {
        portEXIT_CRITICAL(&_configMux);
        release(block);
        return;
    }

    mode = _mode;
    coefficientCount = _coefficientCount;
    resetHistory = _resetHistory;
    _resetHistory = false;

    if (mode == Mode::ACTIVE) {
        std::memcpy(coefficients, _coefficients, coefficientCount * sizeof(short));
    }
    portEXIT_CRITICAL(&_configMux);

    if (mode == Mode::OFF) {
        release(block);
        return;
    }

    if (mode == Mode::BYPASS) {
        transmit(block);
        release(block);
        return;
    }

    if (resetHistory) {
        std::memset(_history, 0, sizeof(_history));
    }

    const size_t historySize = coefficientCount - 1U;
    std::memcpy(_history + historySize, block->data, AUDIO_BLOCK_SAMPLES * sizeof(_history[0]));

    for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        int64_t accumulator = 0;
        const size_t newest = historySize + i;

        for (size_t tap = 0; tap < coefficientCount; ++tap) {
            accumulator += static_cast<int32_t>(
                        coefficients[coefficientCount - tap - 1U]) *
                        static_cast<int32_t>(_history[newest - tap]);
        }

        block->data[i] = q15ToPcm(accumulator);
    }

    if (historySize > 0) {
        std::memmove(_history, _history + AUDIO_BLOCK_SAMPLES, historySize * sizeof(int16_t));
    }

    transmit(block);
    release(block);
}
