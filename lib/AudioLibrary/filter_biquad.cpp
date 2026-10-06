#include "filter_biquad.h"

#include <cmath>
#include <cstring>
#include <limits>

constexpr float BIQUAD_PI = 3.14159265358979323846f;
constexpr float Q30_SCALE = 1073741824.0f; // 2^30

static inline int32_t toQ30(float value) {
    const float scaled = value * Q30_SCALE;

    if (scaled >= 2147483647.0f) {
        return INT32_MAX;
    }

    if (scaled <= -2147483648.0f) {
        return INT32_MIN;
    }

    return (scaled >= 0.0f) ? (int32_t)(scaled + 0.5f) : (int32_t)(scaled - 0.5f);
}

static inline int64_t roundQ30(int64_t value) {
    constexpr int64_t HALF = int64_t{1} << 29;

    if (value >= 0) {
        return (value + HALF) >> 30;
    }

    return -(((-value) + HALF) >> 30);
}

static inline int16_t saturatePcm(int64_t value) {
    if (value > 32767) {
        return 32767;
    }

    if (value < -32768) {
        return -32768;
    }

    return (int16_t)(value);
}

bool validFrequencyAndQ(float frequency, float q) {
    return std::isfinite(frequency) && frequency > 0.0f && frequency < AUDIO_SAMPLE_RATE_EXACT * 0.5f &&
           std::isfinite(q) && q > 0.0f;
}

OSIZE
AudioFilterBiquad::AudioFilterBiquad() : AudioStream(1, inputQueueArray) {
    constexpr int32_t Q30_ONE = int32_t{1} << 30;

    for (uint32_t stage = 0; stage < MAX_STAGES; ++stage) {
        _coefficients[stage] = {Q30_ONE, 0, 0, 0, 0};
    }
}

OSIZE
void AudioFilterBiquad::storeCoefficients(uint32_t stage, const float coefficients[5]) {
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

    const Coefficients converted = {quantized[0], quantized[1], quantized[2], quantized[3], quantized[4]};

    portENTER_CRITICAL(&_configMux);
    _coefficients[stage] = converted;

    if (_stageCount <= stage) {
        _stageCount = (uint8_t)(stage + 1U);
    }

    portEXIT_CRITICAL(&_configMux);
}

OSIZE
void AudioFilterBiquad::setCoefficients(uint32_t stage, const int *coefficients) {
    if (stage >= MAX_STAGES || coefficients == nullptr) {
        return;
    }

    // Die Werte liegen bereits in Q30 vor.
    // Keine Rückwandlung über float/double und damit keine
    // zusätzliche Quantisierung.
    const Coefficients converted = {(int32_t)coefficients[0], (int32_t)coefficients[1], (int32_t)coefficients[2],
                                    (int32_t)coefficients[3], (int32_t)coefficients[4]};

    portENTER_CRITICAL(&_configMux);
    _coefficients[stage] = converted;

    if (_stageCount <= stage) {
        _stageCount = (uint8_t)(stage + 1U);
    }

    portEXIT_CRITICAL(&_configMux);
}

OSIZE
void AudioFilterBiquad::setCoefficients(uint32_t stage, const double *coefficients) {
    if (coefficients == nullptr) {
        return;
    }

    float converted[5];

    for (uint32_t i = 0; i < 5; ++i) {
        if (!std::isfinite(coefficients[i])) {
            return;
        }

        converted[i] = (float)coefficients[i];
    }

    storeCoefficients(stage, converted);
}

OSIZE
void AudioFilterBiquad::setLowpass(uint32_t stage, float frequency, float q) {
    if (!validFrequencyAndQ(frequency, q)) {
        return;
    }

    const float w0 = 2.0f * BIQUAD_PI * frequency / AUDIO_SAMPLE_RATE_EXACT;

    const float sine = std::sin(w0);
    const float cosine = std::cos(w0);
    const float alpha = sine / (2.0f * q);
    const float a0 = 1.0f + alpha;

    const float coefficients[5] = {(1.0f - cosine) * 0.5f / a0, (1.0f - cosine) / a0, (1.0f - cosine) * 0.5f / a0,
                                   -2.0f * cosine / a0, (1.0f - alpha) / a0};

    storeCoefficients(stage, coefficients);
}

OSIZE
void AudioFilterBiquad::setHighpass(uint32_t stage, float frequency, float q) {
    if (!validFrequencyAndQ(frequency, q)) {
        return;
    }

    const float w0 = 2.0f * BIQUAD_PI * frequency / AUDIO_SAMPLE_RATE_EXACT;

    const float sine = std::sin(w0);
    const float cosine = std::cos(w0);
    const float alpha = sine / (2.0f * q);
    const float a0 = 1.0f + alpha;

    const float coefficients[5] = {(1.0f + cosine) * 0.5f / a0, -(1.0f + cosine) / a0, (1.0f + cosine) * 0.5f / a0,
                                   -2.0f * cosine / a0, (1.0f - alpha) / a0};

    storeCoefficients(stage, coefficients);
}

OSIZE
void AudioFilterBiquad::setBandpass(uint32_t stage, float frequency, float q) {
    if (!validFrequencyAndQ(frequency, q)) {
        return;
    }

    const float w0 = 2.0f * BIQUAD_PI * frequency / AUDIO_SAMPLE_RATE_EXACT;

    const float sine = std::sin(w0);
    const float cosine = std::cos(w0);
    const float alpha = sine / (2.0f * q);
    const float a0 = 1.0f + alpha;

    const float coefficients[5] = {alpha / a0, 0.0f, -alpha / a0, -2.0f * cosine / a0, (1.0f - alpha) / a0};

    storeCoefficients(stage, coefficients);
}

OSIZE
void AudioFilterBiquad::setNotch(uint32_t stage, float frequency, float q) {
    if (!validFrequencyAndQ(frequency, q)) {
        return;
    }

    const float w0 = 2.0f * BIQUAD_PI * frequency / AUDIO_SAMPLE_RATE_EXACT;

    const float sine = std::sin(w0);
    const float cosine = std::cos(w0);
    const float alpha = sine / (2.0f * q);
    const float a0 = 1.0f + alpha;

    const float coefficients[5] = {1.0f / a0, -2.0f * cosine / a0, 1.0f / a0, -2.0f * cosine / a0, (1.0f - alpha) / a0};

    storeCoefficients(stage, coefficients);
}

OSIZE
void AudioFilterBiquad::setLowShelf(uint32_t stage, float frequency, float gain, float slope) {
    if (!std::isfinite(frequency) || frequency <= 0.0f || frequency >= AUDIO_SAMPLE_RATE_EXACT * 0.5f ||
        !std::isfinite(gain) || !std::isfinite(slope) || slope <= 0.0f) {
        return;
    }

    const float a = std::pow(10.0f, gain / 40.0f);

    const float w0 = 2.0f * BIQUAD_PI * frequency / AUDIO_SAMPLE_RATE_EXACT;

    const float sine = std::sin(w0);
    const float cosine = std::cos(w0);

    const float radicand = (a + 1.0f / a) * (1.0f / slope - 1.0f) + 2.0f;

    if (radicand < 0.0f) {
        return;
    }

    const float beta = sine * std::sqrt(radicand);
    const float aMinus = (a - 1.0f) * cosine;
    const float aPlus = (a + 1.0f) * cosine;
    const float a0 = (a + 1.0f) + aMinus + beta;

    if (a0 == 0.0f) {
        return;
    }

    const float coefficients[5] = {a * ((a + 1.0f) - aMinus + beta) / a0, 2.0f * a * ((a - 1.0f) - aPlus) / a0,
                                   a * ((a + 1.0f) - aMinus - beta) / a0, -2.0f * ((a - 1.0f) + aPlus) / a0,
                                   ((a + 1.0f) + aMinus - beta) / a0};

    storeCoefficients(stage, coefficients);
}

OSIZE
void AudioFilterBiquad::setHighShelf(uint32_t stage, float frequency, float gain, float slope) {
    if (!std::isfinite(frequency) || frequency <= 0.0f || frequency >= AUDIO_SAMPLE_RATE_EXACT * 0.5f ||
        !std::isfinite(gain) || !std::isfinite(slope) || slope <= 0.0f) {
        return;
    }

    const float a = std::pow(10.0f, gain / 40.0f);

    const float w0 = 2.0f * BIQUAD_PI * frequency / AUDIO_SAMPLE_RATE_EXACT;

    const float sine = std::sin(w0);
    const float cosine = std::cos(w0);

    const float radicand = (a + 1.0f / a) * (1.0f / slope - 1.0f) + 2.0f;

    if (radicand < 0.0f) {
        return;
    }

    const float beta = sine * std::sqrt(radicand);
    const float aMinus = (a - 1.0f) * cosine;
    const float aPlus = (a + 1.0f) * cosine;
    const float a0 = (a + 1.0f) - aMinus + beta;

    if (a0 == 0.0f) {
        return;
    }

    const float coefficients[5] = {a * ((a + 1.0f) + aMinus + beta) / a0, -2.0f * a * ((a - 1.0f) + aPlus) / a0,
                                   a * ((a + 1.0f) + aMinus - beta) / a0, 2.0f * ((a - 1.0f) - aPlus) / a0,
                                   ((a + 1.0f) - aMinus - beta) / a0};

    storeCoefficients(stage, coefficients);
}

OSPEED
void AudioFilterBiquad::update() {
    audio_block_t *block = receiveWritable();

    if (block == nullptr) {
        return;
    }

    Coefficients coefficients[MAX_STAGES];
    uint8_t stageCount;

    portENTER_CRITICAL(&_configMux);
    stageCount = _stageCount;
    memcpy(coefficients, _coefficients, stageCount * sizeof(Coefficients));
    portEXIT_CRITICAL(&_configMux);

    if (stageCount == 0) {
        release(block);
        return;
    }

    // Process the cascade stage by stage instead of sample by sample.
    // This keeps the current stage coefficients/state fixed for the
    // complete block and eliminates the inner stage loop.
    for (uint8_t stage = 0; stage < stageCount; ++stage) {
        const Coefficients &c = coefficients[stage];
        State &state = _state[stage];

        for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            const int16_t input = block->data[i];
            const int64_t outputQ30 = (int64_t)c.b0 * input + state.s1;
            const int16_t output = saturatePcm(roundQ30(outputQ30));
            state.s1 = (int64_t)c.b1 * input + state.s2 - (int64_t)c.a1 * output;
            state.s2 = (int64_t)c.b2 * input - (int64_t)c.a2 * output;
            block->data[i] = output;
        }
    }

    transmit(block);
    release(block);
}
