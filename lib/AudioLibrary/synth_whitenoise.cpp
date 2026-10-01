#include "synth_whitenoise.h"

#include <cmath>

namespace {
constexpr uint32_t PARK_MILLER_MULTIPLIER = 16807U;
constexpr uint32_t PARK_MILLER_MODULUS = 2147483647U;

static inline uint32_t nextNoiseSeed(uint32_t seed) {
    const uint32_t high = PARK_MILLER_MULTIPLIER * (seed >> 16);
    uint32_t low = PARK_MILLER_MULTIPLIER * (seed & 0xFFFFU);

    low += (high & 0x7FFFU) << 16;
    low += high >> 15;
    low = (low & 0x7FFFFFFFU) + (low >> 31);

    return low == PARK_MILLER_MODULUS ? 1U : low;
}

} // namespace

uint32_t AudioSynthNoiseWhite::_instanceCount = 0;

AudioSynthNoiseWhite::AudioSynthNoiseWhite()
    : AudioStream(0, nullptr), _level(0),
      _seed((_instanceCount++ % (PARK_MILLER_MODULUS - 1U)) + 1U) {}

void AudioSynthNoiseWhite::amplitude(float level) {
    if (!std::isfinite(level) || level < 0.0f) {
        level = 0.0f;
    } else if (level > 1.0f) {
        level = 1.0f;
    }

    _level = static_cast<int32_t>(level * 65536.0f + 0.5f);
}

OSPEED
void AudioSynthNoiseWhite::update() {
    const int32_t gain = _level;

    if (gain == 0) {
        return;
    }

    audio_block_t *block = allocate();

    if (block == nullptr) {
        return;
    }

    uint32_t seed = _seed;

    for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        seed = nextNoiseSeed(seed);

        const int32_t noise = static_cast<int32_t>(seed >> 15) - 32768;
        const int32_t scaled = (noise * gain) >> 16;

        block->data[i] = static_cast<int16_t>(scaled);
    }

    _seed = seed;
    transmit(block);
    release(block);
}
