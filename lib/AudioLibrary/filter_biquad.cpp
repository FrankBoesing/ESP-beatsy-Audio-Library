#include "filter_biquad.h"

#include <cmath>
#include <cstring>
#include <limits>

namespace {
constexpr double BIQUAD_PI = 3.14159265358979323846;
constexpr double Q30_SCALE = 1073741824.0;

int32_t toQ30(double value) {
    const double scaled = value * Q30_SCALE;

    if (scaled >= static_cast<double>(std::numeric_limits<int32_t>::max())) {
        return std::numeric_limits<int32_t>::max();
    }

    if (scaled <= static_cast<double>(std::numeric_limits<int32_t>::min())) {
        return std::numeric_limits<int32_t>::min();
    }

    return static_cast<int32_t>(scaled);
}

int64_t roundQ30(int64_t value) {
    constexpr int64_t HALF = int64_t{1} << 29;

    if (value >= 0) {
        return (value + HALF) >> 30;
    }

    return -(((-value) + HALF) >> 30);
}

int16_t saturatePcm(int64_t value) {
    if (value > 32767) {
        return 32767;
    }

    if (value < -32768) {
        return -32768;
    }

    return static_cast<int16_t>(value);
}

bool validFrequencyAndQ(float frequency, float q) {
    return std::isfinite(frequency) && frequency > 0.0f &&
           frequency < AUDIO_SAMPLE_RATE_EXACT * 0.5f && std::isfinite(q) &&
           q > 0.0f;
}
} // namespace

AudioFilterBiquad::AudioFilterBiquad() : AudioStream(1, inputQueueArray) {
    constexpr int32_t Q30_ONE = int32_t{1} << 30;
    for (uint32_t stage = 0; stage < MAX_STAGES; ++stage) {
        _coefficients[stage] = {Q30_ONE, 0, 0, 0, 0};
    }
}

void AudioFilterBiquad::storeCoefficients(uint32_t stage,
                                          const double coefficients[5]) {
    if (stage >= MAX_STAGES || coefficients == nullptr) {
        return;
    }

    int32_t quantized[5];
    for (uint32_t i = 0; i < 5; ++i) {
        if (!std::isfinite(coefficients[i])) {
            return;
        }

        quantized[i] = toQ30(coefficients[i]);
    }

    const Coefficients converted = {
        quantized[0], quantized[1], quantized[2], quantized[3], quantized[4],
    };

    portENTER_CRITICAL(&_configMux);
    _coefficients[stage] = converted;
    if (_stageCount <= stage) {
        _stageCount = static_cast<uint8_t>(stage + 1U);
    }
    portEXIT_CRITICAL(&_configMux);
}

void AudioFilterBiquad::setCoefficients(uint32_t stage,
                                        const int *coefficients) {
    if (coefficients == nullptr) {
        return;
    }

    constexpr double Q30_INVERSE = 1.0 / Q30_SCALE;
    const double converted[5] = {
        static_cast<double>(coefficients[0]) * Q30_INVERSE,
        static_cast<double>(coefficients[1]) * Q30_INVERSE,
        static_cast<double>(coefficients[2]) * Q30_INVERSE,
        static_cast<double>(coefficients[3]) * Q30_INVERSE,
        static_cast<double>(coefficients[4]) * Q30_INVERSE,
    };

    storeCoefficients(stage, converted);
}

void AudioFilterBiquad::setCoefficients(uint32_t stage,
                                        const double *coefficients) {
    storeCoefficients(stage, coefficients);
}

void AudioFilterBiquad::setLowpass(uint32_t stage, float frequency, float q) {
    if (!validFrequencyAndQ(frequency, q)) {
        return;
    }

    const double w0 = 2.0 * BIQUAD_PI * frequency / AUDIO_SAMPLE_RATE_EXACT;
    const double sine = std::sin(w0);
    const double cosine = std::cos(w0);
    const double alpha = sine / (2.0 * q);
    const double a0 = 1.0 + alpha;
    const double coefficients[5] = {
        (1.0 - cosine) * 0.5 / a0, (1.0 - cosine) / a0,
        (1.0 - cosine) * 0.5 / a0, -2.0 * cosine / a0,
        (1.0 - alpha) / a0,
    };

    storeCoefficients(stage, coefficients);
}

void AudioFilterBiquad::setHighpass(uint32_t stage, float frequency, float q) {
    if (!validFrequencyAndQ(frequency, q)) {
        return;
    }

    const double w0 = 2.0 * BIQUAD_PI * frequency / AUDIO_SAMPLE_RATE_EXACT;
    const double sine = std::sin(w0);
    const double cosine = std::cos(w0);
    const double alpha = sine / (2.0 * q);
    const double a0 = 1.0 + alpha;
    const double coefficients[5] = {
        (1.0 + cosine) * 0.5 / a0, -(1.0 + cosine) / a0,
        (1.0 + cosine) * 0.5 / a0, -2.0 * cosine / a0,
        (1.0 - alpha) / a0,
    };

    storeCoefficients(stage, coefficients);
}

void AudioFilterBiquad::setBandpass(uint32_t stage, float frequency, float q) {
    if (!validFrequencyAndQ(frequency, q)) {
        return;
    }

    const double w0 = 2.0 * BIQUAD_PI * frequency / AUDIO_SAMPLE_RATE_EXACT;
    const double sine = std::sin(w0);
    const double cosine = std::cos(w0);
    const double alpha = sine / (2.0 * q);
    const double a0 = 1.0 + alpha;
    const double coefficients[5] = {
        alpha / a0, 0.0, -alpha / a0, -2.0 * cosine / a0, (1.0 - alpha) / a0,
    };

    storeCoefficients(stage, coefficients);
}

void AudioFilterBiquad::setNotch(uint32_t stage, float frequency, float q) {
    if (!validFrequencyAndQ(frequency, q)) {
        return;
    }

    const double w0 = 2.0 * BIQUAD_PI * frequency / AUDIO_SAMPLE_RATE_EXACT;
    const double sine = std::sin(w0);
    const double cosine = std::cos(w0);
    const double alpha = sine / (2.0 * q);
    const double a0 = 1.0 + alpha;
    const double coefficients[5] = {
        1.0 / a0,           -2.0 * cosine / a0, 1.0 / a0,
        -2.0 * cosine / a0, (1.0 - alpha) / a0,
    };

    storeCoefficients(stage, coefficients);
}

void AudioFilterBiquad::setLowShelf(uint32_t stage, float frequency, float gain,
                                    float slope) {
    if (!std::isfinite(frequency) || frequency <= 0.0f ||
        frequency >= AUDIO_SAMPLE_RATE_EXACT * 0.5f || !std::isfinite(gain) ||
        !std::isfinite(slope) || slope <= 0.0f) {
        return;
    }

    const double a = std::pow(10.0, static_cast<double>(gain) / 40.0);
    const double w0 = 2.0 * BIQUAD_PI * frequency / AUDIO_SAMPLE_RATE_EXACT;
    const double sine = std::sin(w0);
    const double cosine = std::cos(w0);
    const double radicand =
        (a + 1.0 / a) * (1.0 / static_cast<double>(slope) - 1.0) + 2.0;

    if (radicand < 0.0) {
        return;
    }

    const double beta = sine * std::sqrt(radicand);
    const double aMinus = (a - 1.0) * cosine;
    const double aPlus = (a + 1.0) * cosine;
    const double a0 = (a + 1.0) + aMinus + beta;

    if (a0 == 0.0) {
        return;
    }

    const double coefficients[5] = {
        a * ((a + 1.0) - aMinus + beta) / a0,
        2.0 * a * ((a - 1.0) - aPlus) / a0,
        a * ((a + 1.0) - aMinus - beta) / a0,
        -2.0 * ((a - 1.0) + aPlus) / a0,
        ((a + 1.0) + aMinus - beta) / a0,
    };

    storeCoefficients(stage, coefficients);
}

void AudioFilterBiquad::setHighShelf(uint32_t stage, float frequency,
                                     float gain, float slope) {
    if (!std::isfinite(frequency) || frequency <= 0.0f ||
        frequency >= AUDIO_SAMPLE_RATE_EXACT * 0.5f || !std::isfinite(gain) ||
        !std::isfinite(slope) || slope <= 0.0f) {
        return;
    }

    const double a = std::pow(10.0, static_cast<double>(gain) / 40.0);
    const double w0 = 2.0 * BIQUAD_PI * frequency / AUDIO_SAMPLE_RATE_EXACT;
    const double sine = std::sin(w0);
    const double cosine = std::cos(w0);
    const double radicand =
        (a + 1.0 / a) * (1.0 / static_cast<double>(slope) - 1.0) + 2.0;

    if (radicand < 0.0) {
        return;
    }

    const double beta = sine * std::sqrt(radicand);
    const double aMinus = (a - 1.0) * cosine;
    const double aPlus = (a + 1.0) * cosine;
    const double a0 = (a + 1.0) - aMinus + beta;

    if (a0 == 0.0) {
        return;
    }

    const double coefficients[5] = {
        a * ((a + 1.0) + aMinus + beta) / a0,
        -2.0 * a * ((a - 1.0) + aPlus) / a0,
        a * ((a + 1.0) + aMinus - beta) / a0,
        2.0 * ((a - 1.0) - aPlus) / a0,
        ((a + 1.0) - aMinus - beta) / a0,
    };

    storeCoefficients(stage, coefficients);
}

void AudioFilterBiquad::update() {
    audio_block_t *block = receiveWritable();

    if (block == nullptr) {
        return;
    }

    Coefficients coefficients[MAX_STAGES];
    uint8_t stageCount;

    portENTER_CRITICAL(&_configMux);
    stageCount = _stageCount;
    std::memcpy(coefficients, _coefficients, stageCount * sizeof(Coefficients));
    portEXIT_CRITICAL(&_configMux);

    if (stageCount == 0) {
        release(block);
        return;
    }

    for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        int16_t value = block->data[i];

        for (uint8_t stage = 0; stage < stageCount; ++stage) {
            const Coefficients &c = coefficients[stage];
            State &state = _state[stage];
            const int16_t stageInput = value;

            const int64_t outputQ30 =
                static_cast<int64_t>(c.b0) * stageInput + state.s1;
            value = saturatePcm(roundQ30(outputQ30));

            state.s1 = static_cast<int64_t>(c.b1) * stageInput + state.s2 -
                       static_cast<int64_t>(c.a1) * value;
            state.s2 = static_cast<int64_t>(c.b2) * stageInput -
                       static_cast<int64_t>(c.a2) * value;
        }

        block->data[i] = value;
    }

    transmit(block);
    release(block);
}
