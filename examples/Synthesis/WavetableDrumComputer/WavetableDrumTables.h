#pragma once

#include <Audio.h>
#include <math.h>
#include <stdint.h>

namespace RockDrumTables {

// Small 256-sample single-cycle tables. The final sample duplicates sample 0
// so AudioSynthWavetable can interpolate safely across the loop boundary.
static int16_t kickWave[257];
static int16_t snareWave[257];
static int16_t tomWave[257];

static void initialize()
{
    constexpr float TWO_PI = 6.28318530718f;

    for (uint16_t i = 0; i < 256; ++i) {
        const float phase = TWO_PI * static_cast<float>(i) / 256.0f;

        // Rounded, harmonic-rich low end. The sketch adds a short pitch drop.
        const float kick =
            0.82f * sinf(phase) +
            0.14f * sinf(2.0f * phase) +
            0.04f * sinf(3.0f * phase);

        // A nasal body for the snare; a separately enveloped noise burst
        // supplies the broadband attack.
        const float snare =
            0.48f * sinf(phase) +
            0.31f * sinf(3.0f * phase) +
            0.21f * sinf(5.0f * phase);

        // A little more upper harmonic content gives the tom some character.
        const float tom =
            0.70f * sinf(phase) +
            0.23f * sinf(2.0f * phase) +
            0.07f * sinf(3.0f * phase);

        kickWave[i] = static_cast<int16_t>(kick * 26000.0f);
        snareWave[i] = static_cast<int16_t>(snare * 22000.0f);
        tomWave[i] = static_cast<int16_t>(tom * 24000.0f);
    }

    kickWave[256] = kickWave[0];
    snareWave[256] = snareWave[0];
    tomWave[256] = tomWave[0];
}

static const uint8_t noteRanges[1] = {127};

// Counts are in groups of eight samples. A high SUSTAIN_MULT gives a
// percussive decay to silence instead of a sustained note.
static const AudioSynthWavetable::sample_data kickSamples[1] = {{
    kickWave,
    true,
    8,
    4294967296.0f / AUDIO_SAMPLE_RATE_EXACT,
    UINT32_MAX,
    0U,
    0U,
    UINT16_MAX,
    0U,
    1U,
    0U,
    920U,       // About 167 ms decay
    1U,
    INT32_MAX,
    UINT32_MAX,
    0U,
    0.0f,
    0.0f,
    UINT32_MAX,
    0U,
    0.0f,
    0.0f,
    0,
    0
}};
static const AudioSynthWavetable::instrument_data kickInstrument = {
    1U, noteRanges, kickSamples
};

static const AudioSynthWavetable::sample_data snareSamples[1] = {{
    snareWave,
    true,
    8,
    4294967296.0f / AUDIO_SAMPLE_RATE_EXACT,
    UINT32_MAX,
    0U,
    0U,
    UINT16_MAX,
    0U,
    1U,
    0U,
    300U,       // About 54 ms decay
    1U,
    INT32_MAX,
    UINT32_MAX,
    0U,
    0.0f,
    0.0f,
    UINT32_MAX,
    0U,
    0.0f,
    0.0f,
    0,
    0
}};
static const AudioSynthWavetable::instrument_data snareInstrument = {
    1U, noteRanges, snareSamples
};

static const AudioSynthWavetable::sample_data tomSamples[1] = {{
    tomWave,
    true,
    8,
    4294967296.0f / AUDIO_SAMPLE_RATE_EXACT,
    UINT32_MAX,
    0U,
    0U,
    UINT16_MAX,
    0U,
    1U,
    0U,
    620U,       // About 113 ms decay
    1U,
    INT32_MAX,
    UINT32_MAX,
    0U,
    0.0f,
    0.0f,
    UINT32_MAX,
    0U,
    0.0f,
    0.0f,
    0,
    0
}};
static const AudioSynthWavetable::instrument_data tomInstrument = {
    1U, noteRanges, tomSamples
};

} // namespace RockDrumTables
