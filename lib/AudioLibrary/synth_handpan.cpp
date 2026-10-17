#include "synth_handpan.h"

#include <math.h>
#include <stdint.h>

#include "utility/dspinst.h"

namespace {
constexpr uint32_t BUILTIN_ATTACK_RATE = 44100;
constexpr uint32_t BUILTIN_ATTACK_COUNT = 1323; // 30 ms at 44.1 kHz
int16_t builtinAttackSample[BUILTIN_ATTACK_COUNT];
bool builtinAttackReady = false;

// Build the default finger-contact transient once, before audio updates start.
// This is generated in RAM by the library; sketches do not need a sample table.
void buildBuiltinAttack()
{
    uint32_t randomState = 0x4d595df4U;
    float lowNoise = 0.0f;
    int32_t peak = 0;

    for (uint32_t i = 0; i < BUILTIN_ATTACK_COUNT; ++i) {
        const float t = (float)i / (float)BUILTIN_ATTACK_RATE;
        randomState = randomState * 1664525U + 1013904223U;
        const float noise = ((float)(randomState >> 8) * (1.0f / 8388607.5f)) - 1.0f;
        lowNoise += 0.12f * (noise - lowNoise);

        float rise = t / 0.0025f;
        if (rise > 1.0f) rise = 1.0f;
        rise = rise * rise * (3.0f - 2.0f * rise);

        const float remaining = (float)(BUILTIN_ATTACK_COUNT - 1U - i) /
                                (0.004f * (float)BUILTIN_ATTACK_RATE);
        float tail = remaining;
        if (tail > 1.0f) tail = 1.0f;
        tail = tail * tail * (3.0f - 2.0f * tail);

        const float noiseBody = 0.42f * lowNoise * expf(-t / 0.0085f);
        const float lowThud = 0.38f * sinf(6.28318530718f * 185.0f * t) *
                              expf(-t / 0.014f);
        const float softKnock = 0.14f * sinf(6.28318530718f * 430.0f * t) *
                                expf(-t / 0.008f);
        const float value = rise * tail * (noiseBody + lowThud + softKnock);
        const int32_t pcm = (int32_t)(value * 32767.0f);
        builtinAttackSample[i] = (int16_t)pcm;
        const int32_t magnitude = pcm < 0 ? -pcm : pcm;
        if (magnitude > peak) peak = magnitude;
    }

    // Intermediate level between the quiet and overly strong listening settings.
    const float scale = peak > 0 ? (0.32f * 32767.0f) / (float)peak : 0.0f;
    for (uint32_t i = 0; i < BUILTIN_ATTACK_COUNT; ++i) {
        builtinAttackSample[i] = (int16_t)((float)builtinAttackSample[i] * scale);
    }
}
} // namespace

AudioSynthHandpan::AudioSynthHandpan() : AudioStream(0, nullptr) {}

bool AudioSynthHandpan::configureNote(uint8_t slot, const ModeConfig *modes,
                                      uint8_t modeCount) {
    if (slot >= MAX_NOTES || modes == nullptr || modeCount == 0 ||
        modeCount > MAX_MODES) {
        return false;
    }

    if (!builtinAttackReady) {
        buildBuiltinAttack();
        builtinAttackReady = true;
    }
    return configureNote(slot, modes, modeCount, builtinAttackSample,
                        BUILTIN_ATTACK_COUNT, BUILTIN_ATTACK_RATE, 0.10f);
}

void AudioSynthHandpan::setSaturation(float amount) {
    if (!isfinite(amount)) {
        return;
    }
    if (amount < 0.0f) amount = 0.0f;
    if (amount > 1.0f) amount = 1.0f;
    saturationAmount = amount;
}

bool AudioSynthHandpan::configureNote(uint8_t slot, const ModeConfig *modes,
                                      uint8_t modeCount,
                                      const int16_t *attackSample,
                                      uint32_t attackSampleCount,
                                      uint32_t attackSampleRate,
                                      float attackGain) {
    if (slot >= MAX_NOTES || modes == nullptr || modeCount == 0 ||
        modeCount > MAX_MODES) {
        return false;
    }

    if ((attackSample == nullptr) != (attackSampleCount == 0)) {
        return false;
    }
    if (attackSampleCount > MAX_ATTACK_SAMPLES) {
        return false;
    }
    if (attackSample != nullptr && (attackSampleCount < 2 || attackSampleRate == 0)) {
        return false;
    }
    if (!isfinite(attackGain) || attackGain < 0.0f || attackGain > 1.0f) {
        return false;
    }

    const float fs = sampleRate();
    if (!isfinite(fs) || fs <= 0.0f) {
        return false;
    }

    Note &note = notes[slot];
    for (uint8_t i = 0; i < MAX_MODES; ++i) {
        note.modes[i] = Resonator{};
    }

    for (uint8_t i = 0; i < modeCount; ++i) {
        const ModeConfig &cfg = modes[i];
        if (!isfinite(cfg.frequencyHz) || cfg.frequencyHz <= 0.0f ||
            cfg.frequencyHz >= fs * 0.48f || !isfinite(cfg.gain) ||
            cfg.gain < 0.0f || cfg.gain > 1.0f ||
            !isfinite(cfg.decaySeconds) || cfg.decaySeconds < 0.005f ||
            cfg.decaySeconds > 10.0f || !isfinite(cfg.phaseRadians)) {
            note.configured = false;
            return false;
        }

        Resonator &mode = note.modes[i];
        mode.config = cfg;

        const float omega = 6.2831853071795864769f * cfg.frequencyHz / fs;
        // r is the per-sample amplitude multiplier for the configured T60.
        const float r = powf(10.0f, -3.0f / (cfg.decaySeconds * fs));
        mode.coefficient1 = 2.0f * r * cosf(omega);
        mode.coefficient2 = -(r * r);
        mode.sinPhase = sinf(cfg.phaseRadians);
        mode.previousStateScale = sinf(cfg.phaseRadians - omega) / r;
    }

    note.modeCount = modeCount;
    note.attackSample = attackSample;
    note.attackSampleCount = attackSampleCount;
    note.attackGain = attackGain;
    note.attackIncrementQ16 = 0;
    const uint32_t rampSamples = (uint32_t)(fs * (float)ONSET_RAMP_US / 1000000.0f + 0.5f);
    note.onsetRampSamples = (uint16_t)(rampSamples < 2 ? 2 : rampSamples);
    if (attackSample != nullptr) {
        const uint64_t increment = ((uint64_t)attackSampleRate << 16) / (uint32_t)(fs + 0.5f);
        if (increment == 0 || increment > UINT32_MAX) {
            note.configured = false;
            return false;
        }
        note.attackIncrementQ16 = (uint32_t)increment;
    }
    note.configured = true;
    return true;
}

bool AudioSynthHandpan::noteOn(uint8_t slot, uint8_t velocity) {
    if (slot >= MAX_NOTES || !notes[slot].configured || velocity == 0) {
        return false;
    }

    portENTER_CRITICAL(&eventMux);
    const uint8_t nextWrite = (uint8_t)((eventWrite + 1U) % MAX_PENDING_EVENTS);
    if (nextWrite == eventRead) {
        portEXIT_CRITICAL(&eventMux);
        return false;
    }

    eventQueue[eventWrite].slot = slot;
    eventQueue[eventWrite].velocity = velocity;
    eventWrite = nextWrite;
    portEXIT_CRITICAL(&eventMux);
    return true;
}

bool AudioSynthHandpan::popEvent(NoteEvent &event) {
    bool available = false;
    portENTER_CRITICAL(&eventMux);
    if (eventRead != eventWrite) {
        event = eventQueue[eventRead];
        eventRead = (uint8_t)((eventRead + 1U) % MAX_PENDING_EVENTS);
        available = true;
    }
    portEXIT_CRITICAL(&eventMux);
    return available;
}

void AudioSynthHandpan::startAttack(uint8_t slot, float velocity) {
    const Note &note = notes[slot];
    if (note.attackSample == nullptr || note.attackSampleCount < 2 ||
        note.attackGain <= 0.0f) {
        return;
    }

    int selected = -1;
    uint32_t shortestRemaining = UINT32_MAX;
    for (uint8_t i = 0; i < MAX_ATTACK_VOICES; ++i) {
        AttackVoice &voice = attackVoices[i];
        if (!voice.active) {
            selected = (int)i;
            break;
        }
        const uint32_t endQ16 = voice.sampleCount << 16;
        const uint32_t remaining = voice.positionQ16 < endQ16
                                       ? endQ16 - voice.positionQ16
                                       : 0;
        if (remaining < shortestRemaining) {
            shortestRemaining = remaining;
            selected = (int)i;
        }
    }

    if (selected < 0) {
        return;
    }
    AttackVoice &voice = attackVoices[selected];
    voice.sample = note.attackSample;
    voice.sampleCount = note.attackSampleCount;
    voice.positionQ16 = 0;
    voice.incrementQ16 = note.attackIncrementQ16;
    voice.gain = note.attackGain * velocity;
    voice.active = voice.incrementQ16 != 0 && voice.gain > 0.0f;
}

void AudioSynthHandpan::triggerNote(uint8_t slot, uint8_t velocity) {
    if (slot >= MAX_NOTES || !notes[slot].configured || velocity == 0) {
        return;
    }

    Note &note = notes[slot];
    const float velocityScale = (float)velocity * (1.0f / 127.0f);

    // Start a separate resonator voice for this strike. Its contribution is
    // smoothly faded in over a few milliseconds; once the ramp ends, the state
    // is merged into the pad's long-lived resonators. Existing tails keep ringing.
    int selected = -1;
    for (uint8_t i = 0; i < MAX_STRIKE_VOICES; ++i) {
        if (!strikeVoices[i].active) {
            selected = (int)i;
            break;
        }
    }
    if (selected < 0) {
        // Avoid an abrupt fallback excitation if the strike pool is exhausted.
        return;
    }

    StrikeVoice &voice = strikeVoices[selected];
    voice.slot = slot;
    voice.position = 0;
    voice.active = true;
    activeStrikeVoiceIndices[activeStrikeVoiceCount++] = (uint8_t)selected;
    for (uint8_t i = 0; i < MAX_MODES; ++i) {
        voice.y1[i] = 0.0f;
        voice.y2[i] = 0.0f;
    }

    for (uint8_t i = 0; i < note.modeCount; ++i) {
        const Resonator &mode = note.modes[i];
        if (mode.config.gain <= 0.0f) {
            continue;
        }

        const float amplitude = mode.config.gain * velocityScale;
        voice.y1[i] = amplitude * mode.sinPhase;
        voice.y2[i] = amplitude * mode.previousStateScale;
    }

    startAttack(slot, velocityScale);
}

bool AudioSynthHandpan::hasActiveSound() const {
    if (activeModeCount != 0) {
        return true;
    }
    if (activeStrikeVoiceCount != 0) {
        return true;
    }
    for (uint8_t i = 0; i < MAX_ATTACK_VOICES; ++i) {
        if (attackVoices[i].active) {
            return true;
        }
    }
    return false;
}

OSPEED
void AudioSynthHandpan::update() {
    NoteEvent event;
    while (popEvent(event)) {
        triggerNote(event.slot, event.velocity);
    }

    if (!hasActiveSound()) {
        return;
    }

    // Allocate once per block. If unavailable, DSP state still advances so a
    // temporary audio-pool shortage does not stretch note tails in time.
    audio_block_t *block = allocate();
    const float saturation = saturationAmount;

    for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        mixBuffer[i] = 0.0f;
    }

    // Process one resonator for an entire block. Coefficients and the two
    // delay states stay in local variables, and quiet-mode detection happens
    // once per block instead of once per mode per sample. This is the hot path
    // that matters when many pads are ringing simultaneously.
    uint8_t activeIndex = 0;
    while (activeIndex < activeModeCount) {
        const ActiveModeRef ref = activeModes[activeIndex];
        Resonator &mode = notes[ref.slot].modes[ref.mode];

        const float coefficient1 = mode.coefficient1;
        const float coefficient2 = mode.coefficient2;
        float y1 = mode.y1;
        float y2 = mode.y2;

        for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            mixBuffer[i] += y1;

            const float next = coefficient1 * y1 + coefficient2 * y2;
            y2 = y1;
            y1 = next;
        }

        mode.y1 = y1;
        mode.y2 = y2;

        // A stable damped resonator will not regrow after both state variables
        // have become this small. Block-level culling avoids per-sample fabsf
        // calls and dynamic-list mutations in the inner loop.
        if (fabsf(y1) < SILENCE_THRESHOLD &&
            fabsf(y2) < SILENCE_THRESHOLD) {
            mode.y1 = 0.0f;
            mode.y2 = 0.0f;
            mode.active = false;
            --activeModeCount;
            activeModes[activeIndex] = activeModes[activeModeCount];
        } else {
            ++activeIndex;
        }
    }

    // Attack samples are short and limited to eight voices, so sample-major
    // playback is inexpensive. It is also processed when block allocation
    // failed, preserving the sample/decay timeline.
    for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        float mix = mixBuffer[i];

        // Render the newly triggered resonator voices with a smoothstep
        // envelope. The short onset fade avoids a discontinuity, while the
        // independent states let existing pad tails ring without being ducked.
        for (uint8_t activeStrikeIndex = 0;
             activeStrikeIndex < activeStrikeVoiceCount; ++activeStrikeIndex) {
            StrikeVoice &voice = strikeVoices[activeStrikeVoiceIndices[activeStrikeIndex]];

            const Note &note = notes[voice.slot];
            const uint16_t rampSamples = note.onsetRampSamples;
            float envelope = 1.0f;
            if (voice.position < rampSamples) {
                const float x = (float)voice.position / (float)(rampSamples - 1U);
                envelope = x * x * (3.0f - 2.0f * x);
            }

            for (uint8_t m = 0; m < note.modeCount; ++m) {
                const Resonator &mode = note.modes[m];
                if (mode.config.gain <= 0.0f) {
                    continue;
                }

                mix += voice.y1[m] * envelope;
                const float next = mode.coefficient1 * voice.y1[m] +
                                   mode.coefficient2 * voice.y2[m];
                voice.y2[m] = voice.y1[m];
                voice.y1[m] = next;
            }

            if (voice.position < rampSamples) {
                ++voice.position;
            }
        }

        for (uint8_t v = 0; v < MAX_ATTACK_VOICES; ++v) {
            AttackVoice &voice = attackVoices[v];
            if (!voice.active) {
                continue;
            }

            const uint32_t index = voice.positionQ16 >> 16;
            if (index >= voice.sampleCount) {
                voice.active = false;
                continue;
            }

            const uint32_t nextIndex =
                (index + 1U < voice.sampleCount) ? index + 1U : index;
            const int32_t sample0 = voice.sample[index];
            const int32_t sample1 = voice.sample[nextIndex];
            const uint32_t fraction = voice.positionQ16 & 0xFFFFU;
            // fraction >> 1 keeps the signed product within int32_t even
            // for a full-scale int16_t transition. The lost LSB of the Q16
            // phase fraction changes the interpolated value by less than one
            // PCM count and avoids a costly 64-bit multiply on ESP32 Gen 1.
            const int32_t interpolated =
                sample0 + (((sample1 - sample0) * (int32_t)(fraction >> 1)) >> 15);

            mix += ((float)interpolated * (1.0f / 32768.0f)) * voice.gain;
            voice.positionQ16 += voice.incrementQ16;

            if ((voice.positionQ16 >> 16) >= voice.sampleCount) {
                voice.active = false;
            }
        }

        if (block != nullptr) {
            if (saturation > 0.0f) {
                // Match the H2 offline curve: tanh(1.8 * mix) / 1.8.
                // The rational approximation below is close in the normalized
                // audio range and avoids a costly tanhf() call per sample.
                const float driven = mix * 1.8f;
                const float driven2 = driven * driven;
                float shaped = (1.0f / 1.8f) * driven *
                               (27.0f + driven2) / (27.0f + 9.0f * driven2);

                // Keep the approximation bounded at the tanh limit for overloads.
                if (shaped > 0.5555556f) shaped = 0.5555556f;
                if (shaped < -0.5555556f) shaped = -0.5555556f;
                mix += saturation * (shaped - mix);
            }

            if (mix > 1.0f) mix = 1.0f;
            if (mix < -1.0f) mix = -1.0f;
            block->data[i] = saturate16((int32_t)(mix * OUTPUT_SCALE));
        }
    }

    // At the end of the block, transfer fully ramped strike states into the
    // shared pad resonators. Both sets of states are now at the same time point.
    uint8_t activeStrikeIndex = 0;
    while (activeStrikeIndex < activeStrikeVoiceCount) {
        const uint8_t voiceIndex = activeStrikeVoiceIndices[activeStrikeIndex];
        StrikeVoice &voice = strikeVoices[voiceIndex];
        Note &note = notes[voice.slot];
        if (voice.position < note.onsetRampSamples) {
            ++activeStrikeIndex;
            continue;
        }

        for (uint8_t m = 0; m < note.modeCount; ++m) {
            Resonator &mode = note.modes[m];
            if (mode.config.gain <= 0.0f) {
                continue;
            }

            mode.y1 += voice.y1[m];
            mode.y2 += voice.y2[m];
            if (!mode.active && activeModeCount < (MAX_NOTES * MAX_MODES)) {
                activeModes[activeModeCount].slot = voice.slot;
                activeModes[activeModeCount].mode = m;
                ++activeModeCount;
                mode.active = true;
            }
        }
        voice.active = false;
        --activeStrikeVoiceCount;
        activeStrikeVoiceIndices[activeStrikeIndex] =
            activeStrikeVoiceIndices[activeStrikeVoiceCount];
    }

    if (block != nullptr) {
        transmit(block, 0);
        release(block);
    }
}
