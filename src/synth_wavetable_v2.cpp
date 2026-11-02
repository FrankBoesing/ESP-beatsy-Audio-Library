/*
 * ESP-beatsy Audio Library
 *
 * Polyphonic, zone-based sample wavetable synthesizer.
 */

#include "synth_wavetable_v2.h"

#include <limits.h>
#include <math.h>
#include <string.h>

#if defined(__XTENSA__)
#include <xtensa/config/core-isa.h>
#endif

// Keep the Q16 gain conversion exact without a 64-bit multiply.
// Splitting the signed input into a signed high word and an unsigned low word
// gives: floor((value * gain) / 65536) = high * gain + ((low * gain) >> 16).
static inline int32_t wavetableV2ScaleQ16(int32_t value, uint16_t gain) {
    const uint32_t bits = (uint32_t)value;
    const int32_t high =
        (int32_t)(bits >> 16) - (value < 0 ? 65536 : 0);
    const uint32_t low = bits & 0xFFFFU;

    return high * (int32_t)gain +
           (int32_t)((low * (uint32_t)gain) >> 16);
}

// Xtensa LX7 (ESP32-S3): CLAMPS performs an exact signed 16-bit clamp.
static inline int16_t wavetableV2Saturate16(int32_t value) {
#if defined(__XTENSA__) && defined(XCHAL_HAVE_CLAMPS) && XCHAL_HAVE_CLAMPS
    int32_t clamped;
    __asm__ volatile(
        "clamps %0, %1, 15"
        : "=a"(clamped)
        : "a"(value));
    return (int16_t)clamped;
#else
    if (value > 32767) return 32767;
    if (value < -32768) return -32768;
    return (int16_t)value;
#endif
}

#if defined(__riscv) && defined(CONFIG_IDF_TARGET_ESP32P4) && defined(__riscv_xespv)
// ESP32-P4 XespV: clamp four signed 32-bit values in parallel. The temporary
// and bound vectors are 16-byte aligned because VLD.128 requires alignment.
// MOVI.16.A extracts the low half-word of each clamped 32-bit lane. This runs
// in AudioTask task context; ESP-IDF FreeRTOS manages PIE lazy context switching.
// Do not call this helper from an ISR.
static inline void wavetableV2ScaleSaturate4P4(const int32_t *input,
                                                uint16_t gain,
                                                int16_t *output) {
    alignas(16) int32_t scaled[4] = {
        wavetableV2ScaleQ16(input[0], gain),
        wavetableV2ScaleQ16(input[1], gain),
        wavetableV2ScaleQ16(input[2], gain),
        wavetableV2ScaleQ16(input[3], gain)
    };

    alignas(16) static const int32_t lowerBound[4] = {
        -32768, -32768, -32768, -32768
    };
    alignas(16) static const int32_t upperBound[4] = {
        32767, 32767, 32767, 32767
    };

    // Let GCC allocate scalar registers; early-clobber keeps the
    // post-increment pointers distinct from the extracted output values.
    const int32_t *scaledPtr = scaled;
    const int32_t *lowerPtr = lowerBound;
    const int32_t *upperPtr = upperBound;
    uint32_t out0, out1, out2, out3;

    __asm__ volatile(
        "esp.vld.128.ip q0, %[scaled], 16\n"
        "esp.vld.128.ip q1, %[lower], 16\n"
        "esp.vmax.s32 q2, q0, q1\n"
        "esp.vld.128.ip q1, %[upper], 16\n"
        "esp.vmin.s32 q2, q2, q1\n"
        "esp.movi.16.a q2, %[out0], 0\n"
        "esp.movi.16.a q2, %[out1], 2\n"
        "esp.movi.16.a q2, %[out2], 4\n"
        "esp.movi.16.a q2, %[out3], 6\n"
        : [scaled] "+&r"(scaledPtr),
          [lower] "+&r"(lowerPtr),
          [upper] "+&r"(upperPtr),
          [out0] "=&r"(out0),
          [out1] "=&r"(out1),
          [out2] "=&r"(out2),
          [out3] "=&r"(out3)
        :
        : "q0", "q1", "q2", "memory");

    output[0] = (int16_t)out0;
    output[1] = (int16_t)out1;
    output[2] = (int16_t)out2;
    output[3] = (int16_t)out3;
}
#endif

// MIDI note frequencies in Q16.16 Hz.
static const uint32_t wavetableV2NoteFrequencyQ16[128] = {
    535809U, 567670U, 601425U, 637188U, 675077U, 715219U, 757749U, 802807U,
    850544U, 901120U, 954703U, 1011473U, 1071618U, 1135340U, 1202851U, 1274376U,
    1350154U, 1430439U, 1515497U, 1605613U, 1701088U, 1802240U, 1909407U, 2022946U,
    2143237U, 2270680U, 2405702U, 2548752U, 2700309U, 2860878U, 3030994U, 3211227U,
    3402176U, 3604480U, 3818814U, 4045892U, 4286473U, 4541360U, 4811404U, 5097505U,
    5400618U, 5721755U, 6061989U, 6422453U, 6804352U, 7208960U, 7637627U, 8091784U,
    8572947U, 9082720U, 9622807U, 10195009U, 10801236U, 11443511U, 12123977U, 12844906U,
    13608704U, 14417920U, 15275254U, 16183568U, 17145893U, 18165441U, 19245614U, 20390018U,
    21602472U, 22887021U, 24247954U, 25689813U, 27217409U, 28835840U, 30550508U, 32367136U,
    34291786U, 36330882U, 38491228U, 40780036U, 43204943U, 45774043U, 48495909U, 51379626U,
    54434817U, 57671680U, 61101017U, 64734272U, 68583572U, 72661764U, 76982457U, 81560072U,
    86409886U, 91548086U, 96991818U, 102759252U, 108869635U, 115343360U, 122202033U, 129468544U,
    137167144U, 145323527U, 153964914U, 163120144U, 172819773U, 183096171U, 193983636U, 205518503U,
    217739269U, 230686720U, 244404066U, 258937088U, 274334289U, 290647054U, 307929828U, 326240288U,
    345639545U, 366192342U, 387967272U, 411037006U, 435478539U, 461373440U, 488808132U, 517874176U,
    548668578U, 581294109U, 615859655U, 652480576U, 691279090U, 732384684U, 775934544U, 822074013U
};

static inline uint32_t wavetableV2PhaseIncrement(uint32_t frequencyQ16,
                                                float perHertzPhaseIncrement,
                                                float phaseScale) {
    const float coefficient = perHertzPhaseIncrement * phaseScale;
    if (!isfinite(coefficient) || coefficient <= 0.0f) return 0;

    // Convert the per-Hertz coefficient to Q16.16. The product is Q32.32.
    const float coefficientQ16Float = coefficient * 65536.0f;
    if (!isfinite(coefficientQ16Float) ||
        coefficientQ16Float >= 1.8446744073709552e19f) return UINT32_MAX;

    const uint64_t coefficientQ16 = (uint64_t)(coefficientQ16Float + 0.5f);
    if (coefficientQ16 > UINT64_MAX / frequencyQ16) return UINT32_MAX;

    const uint64_t product = (uint64_t)frequencyQ16 * coefficientQ16;
    if (product > UINT64_MAX - 0x80000000ULL) return UINT32_MAX;

    const uint64_t increment = (product + 0x80000000ULL) >> 32;
    if (increment == 0) return 1;
    return increment >= UINT32_MAX ? UINT32_MAX : (uint32_t)increment;
}

bool AudioSynthWavetableV2::setInstrument(const instrument_data &newInstrument) {
    if (newInstrument.ZONE_COUNT != 0 && newInstrument.ZONES == nullptr) {
        return false;
    }

    for (uint16_t i = 0; i < newInstrument.ZONE_COUNT; ++i) {
        const zone_data &zone = newInstrument.ZONES[i];
        if (zone.KEY_LOW > zone.KEY_HIGH || zone.VELOCITY_LOW > zone.VELOCITY_HIGH ||
            zone.SAMPLE == nullptr) {
            return false;
        }

        const sample_data &sample = *zone.SAMPLE;
        if (sample.SAMPLE == nullptr || sample.SAMPLE_COUNT < 2 ||
            sample.INDEX_BITS == 0 || sample.INDEX_BITS > 16 ||
            sample.SAMPLE_COUNT > ((1UL << sample.INDEX_BITS) + 1UL) ||
            !isfinite(sample.PER_HERTZ_PHASE_INCREMENT) ||
            sample.PER_HERTZ_PHASE_INCREMENT <= 0.0f) {
            return false;
        }

        if (sample.LOOP_MODE != LoopMode::ONE_SHOT &&
            sample.LOOP_MODE != LoopMode::CONTINUOUS &&
            sample.LOOP_MODE != LoopMode::UNTIL_RELEASE) {
            return false;
        }

        const uint64_t phaseCapacity =
            (uint64_t)(sample.SAMPLE_COUNT - 1U) << (32U - sample.INDEX_BITS);
        if (sample.MAX_PHASE != 0 && (uint64_t)sample.MAX_PHASE > phaseCapacity) {
            return false;
        }

        if (sample.LOOP_PHASE_LENGTH != 0) {
            if (sample.LOOP_PHASE_END < sample.LOOP_PHASE_LENGTH) return false;
            uint64_t maximum = sample.MAX_PHASE != 0 ? sample.MAX_PHASE : phaseCapacity;
            if (maximum > UINT32_MAX) maximum = UINT32_MAX;
            if (sample.LOOP_PHASE_END > maximum) return false;
        }
    }

    float currentSampleRate = sampleRate();
    if (!isfinite(currentSampleRate) || currentSampleRate <= 0.0f) {
        currentSampleRate = AUDIO_SAMPLE_RATE_EXACT;
    }

    portENTER_CRITICAL(&stateMux_);
    instrument_ = &newInstrument;
    sampleCountScale_ = currentSampleRate / AUDIO_SAMPLE_RATE_EXACT;
    phaseIncrementScale_ = AUDIO_SAMPLE_RATE_EXACT / currentSampleRate;

    for (uint8_t i = 0; i < MAX_NOTES; ++i) {
        notes_[i] = NoteState{};
    }
    for (uint16_t i = 0; i < MAX_SAMPLE_VOICES; ++i) {
        sampleVoices_[i] = SampleVoice{};
    }
    activeVoiceCount_ = 0;
    activeStereoVoiceCount_ = 0;
    for (uint8_t i = 0; i < 16; ++i) {
        sustain_[i] = false;
    }
    ++nextToken_;
    portEXIT_CRITICAL(&stateMux_);
    return true;
}

uint32_t AudioSynthWavetableV2::scaleSampleCount(uint32_t value) const {
    const double scaled = (double)value * (double)sampleCountScale_ + 0.5;
    return scaled >= (double)INT32_MAX ? (uint32_t)INT32_MAX : (uint32_t)scaled;
}

uint32_t AudioSynthWavetableV2::maximumPhase(const sample_data &sample) const {
    if (sample.MAX_PHASE != 0) {
        return sample.MAX_PHASE;
    }

    const uint64_t validFrames = sample.SAMPLE_COUNT - 1U;
    const uint64_t derived = validFrames << (32U - sample.INDEX_BITS);
    return derived >= (uint64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)derived;
}

float AudioSynthWavetableV2::noteToFreq(int note) {
    if (note < 0) note = 0;
    if (note > 127) note = 127;
    return (float)wavetableV2NoteFrequencyQ16[note] * (1.0f / 65536.0f);
}

uint8_t AudioSynthWavetableV2::noteOn(int note, int velocity, uint8_t channel) {
    if (note < 0) note = 0;
    if (note > 127) note = 127;
    if (velocity < 0) velocity = 0;
    if (velocity > 127) velocity = 127;
    if (channel > 15) return 0;

    if (velocity == 0) {
        noteOff(note, channel);
        return 0;
    }

    // Read the instrument and sample-rate scale under the control lock, then
    // resolve zones and calculate phase increments outside the critical section.
    const instrument_data *instrument;
    float phaseScale;
    portENTER_CRITICAL(&stateMux_);
    instrument = instrument_;
    phaseScale = phaseIncrementScale_;
    portEXIT_CRITICAL(&stateMux_);

    if (instrument == nullptr || instrument->ZONES == nullptr ||
        instrument->ZONE_COUNT == 0) {
        return 0;
    }

    const sample_data *matchingSamples[MAX_SAMPLE_LAYERS_PER_NOTE];
    uint8_t layerCount = 0;
    for (uint16_t i = 0; i < instrument->ZONE_COUNT; ++i) {
        const zone_data &zone = instrument->ZONES[i];
        if (note >= zone.KEY_LOW && note <= zone.KEY_HIGH &&
            velocity >= zone.VELOCITY_LOW && velocity <= zone.VELOCITY_HIGH) {
            if (layerCount < MAX_SAMPLE_LAYERS_PER_NOTE) {
                matchingSamples[layerCount++] = zone.SAMPLE;
            } else {
                break;
            }
        }
    }

    if (layerCount == 0) return 0;

    const uint32_t frequencyQ16 = wavetableV2NoteFrequencyQ16[note];
    uint32_t phaseIncrements[MAX_SAMPLE_LAYERS_PER_NOTE];
    for (uint8_t layer = 0; layer < layerCount; ++layer) {
        phaseIncrements[layer] = wavetableV2PhaseIncrement(
            frequencyQ16, matchingSamples[layer]->PER_HERTZ_PHASE_INCREMENT,
            phaseScale);
    }

    portENTER_CRITICAL(&stateMux_);
    if (instrument_ != instrument) {
        portEXIT_CRITICAL(&stateMux_);
        return 0;
    }

    int noteSlot = -1;
    for (uint8_t i = 0; i < MAX_NOTES; ++i) {
        if (!notes_[i].active) {
            noteSlot = i;
            break;
        }
    }

    if (noteSlot < 0) {
        // Prefer stealing the oldest released/sustain-held note. If every note
        // is still held, steal the oldest note as a last resort.
        uint32_t oldestAge = UINT32_MAX;
        for (uint8_t i = 0; i < MAX_NOTES; ++i) {
            if (!notes_[i].keyDown && notes_[i].age <= oldestAge) {
                oldestAge = notes_[i].age;
                noteSlot = i;
            }
        }
        if (noteSlot < 0) {
            oldestAge = UINT32_MAX;
            for (uint8_t i = 0; i < MAX_NOTES; ++i) {
                if (notes_[i].age <= oldestAge) {
                    oldestAge = notes_[i].age;
                    noteSlot = i;
                }
            }
        }
        if (noteSlot >= 0) {
            stealNoteLocked((uint8_t)noteSlot);
        }
    }

    if (noteSlot < 0) {
        portEXIT_CRITICAL(&stateMux_);
        return 0;
    }

    uint16_t freeSlots[MAX_SAMPLE_LAYERS_PER_NOTE];
    uint8_t freeCount = 0;
    for (uint16_t i = 0; i < MAX_SAMPLE_VOICES && freeCount < layerCount; ++i) {
        if (!sampleVoices_[i].active) {
            freeSlots[freeCount++] = i;
        }
    }

    if (freeCount != layerCount) {
        portEXIT_CRITICAL(&stateMux_);
        return 0;
    }

    uint32_t token = ++nextToken_;
    if (token == 0) token = ++nextToken_;

    NoteState &noteState = notes_[noteSlot];
    noteState.active = true;
    noteState.keyDown = true;
    noteState.sustainHeld = false;
    noteState.note = (uint8_t)note;
    noteState.channel = channel;
    noteState.layerCount = 0;
    noteState.token = token;
    noteState.age = ++nextAge_;

    const uint32_t velocityGain = ((uint32_t)velocity * UINT16_MAX + 63U) / 127U;
    uint8_t startedLayers = 0;

    for (uint8_t layer = 0; layer < layerCount; ++layer) {
        const uint16_t slot = freeSlots[layer];
        SampleVoice &voice = sampleVoices_[slot];
        const sample_data *sample = matchingSamples[layer];

        voice = SampleVoice{};
        voice.active = true;
        voice.released = false;
        voice.noteSlot = (uint8_t)noteSlot;
        voice.noteToken = token;
        voice.sample = sample;
        voice.phase = 0;
        voice.phaseIncrement = phaseIncrements[layer];

        if (voice.phaseIncrement == 0 || activeVoiceCount_ >= MAX_SAMPLE_VOICES) {
            voice.active = false;
            continue;
        }

        activeVoiceSlots_[activeVoiceCount_++] = slot;
        if (sample->SAMPLE_RIGHT != nullptr) ++activeStereoVoiceCount_;
        ++startedLayers;

        voice.amplitude = (uint16_t)(((uint64_t)velocityGain *
                                      sample->INITIAL_ATTENUATION_SCALAR) >> 16);

        voice.envelopeState = STATE_DELAY;
        voice.envelopeCount = (int32_t)scaleSampleCount(sample->DELAY_COUNT);
        voice.envelopeMultiplier = 0;
        voice.envelopeIncrement = 0;
        voice.envelopeSubCount = 0;

        voice.delayCount = scaleSampleCount(sample->DELAY_COUNT);
        voice.attackCount = scaleSampleCount(sample->ATTACK_COUNT);
        voice.holdCount = scaleSampleCount(sample->HOLD_COUNT);
        voice.decayCount = scaleSampleCount(sample->DECAY_COUNT);
        voice.releaseCount = scaleSampleCount(sample->RELEASE_COUNT);
    }

    noteState.layerCount = startedLayers;
    if (startedLayers == 0) {
        noteState = NoteState{};
    }

    portEXIT_CRITICAL(&stateMux_);
    return startedLayers;
}

void AudioSynthWavetableV2::beginReleaseLocked(SampleVoice &voice) {
    if (!voice.active) return;

    voice.released = true;
    if (voice.envelopeState == STATE_IDLE || voice.envelopeState == STATE_RELEASE) {
        return;
    }

    uint32_t releaseCount = voice.releaseCount;
    if (releaseCount == 0) releaseCount = 1;

    voice.envelopeState = STATE_RELEASE;
    voice.envelopeCount = (int32_t)releaseCount;
    voice.envelopeSubCount = 0;

    const int64_t denominator = (int64_t)releaseCount * ENVELOPE_PERIOD;
    voice.envelopeIncrement = denominator > 0
                                  ? (int32_t)(-(int64_t)voice.envelopeMultiplier /
                                              denominator)
                                  : 0;

    // Precompute the amplitude-scaled release ramp. The quotient/remainder
    // pair preserves the exact product decrement without a 64-bit multiply
    // in the per-sample render loop.
    const uint32_t releaseStep = voice.envelopeIncrement < 0
                                     ? (uint32_t)(-(int64_t)voice.envelopeIncrement)
                                     : 0U;
    const uint64_t gainProduct =
        (uint64_t)voice.amplitude * (uint32_t)voice.envelopeMultiplier;
    const uint64_t gainStepProduct =
        (uint64_t)voice.amplitude * releaseStep;

    voice.releaseGainQ16_16 = (uint32_t)(gainProduct >> 15);
    voice.releaseGainRemainder = (uint16_t)(gainProduct & 0x7FFFU);
    voice.releaseGainStepQ16_16 = (uint32_t)(gainStepProduct >> 15);
    voice.releaseGainStepRemainder = (uint16_t)(gainStepProduct & 0x7FFFU);

}

void AudioSynthWavetableV2::compactInactiveVoices() {
    uint16_t write = 0;
    for (uint16_t read = 0; read < activeVoiceCount_; ++read) {
        const uint16_t slot = activeVoiceSlots_[read];
        if (sampleVoices_[slot].active) {
            activeVoiceSlots_[write++] = slot;
        } else if (sampleVoices_[slot].sample != nullptr &&
                   sampleVoices_[slot].sample->SAMPLE_RIGHT != nullptr) {
            --activeStereoVoiceCount_;
        }
    }
    activeVoiceCount_ = write;
}

void AudioSynthWavetableV2::releaseNoteLocked(uint8_t noteSlot) {
    if (noteSlot >= MAX_NOTES || !notes_[noteSlot].active) return;

    const uint32_t token = notes_[noteSlot].token;
    for (uint16_t i = 0; i < activeVoiceCount_; ++i) {
        SampleVoice &voice = sampleVoices_[activeVoiceSlots_[i]];
        if (voice.noteSlot == noteSlot && voice.noteToken == token) {
            beginReleaseLocked(voice);
        }
    }
}

void AudioSynthWavetableV2::stealNoteLocked(uint8_t noteSlot) {
    if (noteSlot >= MAX_NOTES || !notes_[noteSlot].active) return;

    const uint32_t token = notes_[noteSlot].token;
    for (uint16_t i = 0; i < activeVoiceCount_;) {
        const uint16_t slot = activeVoiceSlots_[i];
        SampleVoice &voice = sampleVoices_[slot];
        if (voice.noteSlot == noteSlot && voice.noteToken == token) {
            voice.active = false;
            if (voice.sample != nullptr && voice.sample->SAMPLE_RIGHT != nullptr) {
                --activeStereoVoiceCount_;
            }
            activeVoiceSlots_[i] = activeVoiceSlots_[--activeVoiceCount_];
        } else {
            ++i;
        }
    }
    notes_[noteSlot] = NoteState{};
}

bool AudioSynthWavetableV2::noteOff(int note, uint8_t channel) {
    if (note < 0 || note > 127 || channel > 15) return false;

    portENTER_CRITICAL(&stateMux_);

    int noteSlot = -1;
    uint32_t oldestAge = UINT32_MAX;
    for (uint8_t i = 0; i < MAX_NOTES; ++i) {
        const NoteState &state = notes_[i];
        if (state.active && state.keyDown && state.note == (uint8_t)note &&
            state.channel == channel && state.age <= oldestAge) {
            oldestAge = state.age;
            noteSlot = i;
        }
    }

    if (noteSlot < 0) {
        portEXIT_CRITICAL(&stateMux_);
        return false;
    }

    NoteState &state = notes_[noteSlot];
    state.keyDown = false;
    if (sustain_[channel]) {
        state.sustainHeld = true;
    } else {
        state.sustainHeld = false;
        releaseNoteLocked((uint8_t)noteSlot);
    }

    portEXIT_CRITICAL(&stateMux_);
    return true;
}

void AudioSynthWavetableV2::allNotesOff(int channel) {
    if (channel < -1 || channel > 15) return;

    portENTER_CRITICAL(&stateMux_);

    // Mark notes that need a release. A bit per note lets us match all their
    // sample layers in one pass over the active voice list below.
    uint32_t releaseNoteMask = 0;

    for (uint8_t i = 0; i < MAX_NOTES; ++i) {
        NoteState &state = notes_[i];
        if (!state.active || (channel >= 0 && state.channel != (uint8_t)channel)) {
            continue;
        }

        state.keyDown = false;
        if (sustain_[state.channel]) {
            state.sustainHeld = true;
        } else {
            state.sustainHeld = false;
            releaseNoteMask |= (1UL << i);
        }
    }

    // Avoid rescanning every active voice once per released note. Each voice
    // is visited once, and its note token prevents matching a reused note slot.
    if (releaseNoteMask != 0) {
        for (uint16_t i = 0; i < activeVoiceCount_; ++i) {
            SampleVoice &voice = sampleVoices_[activeVoiceSlots_[i]];
            const uint8_t noteSlot = voice.noteSlot;

            if (noteSlot < MAX_NOTES &&
                (releaseNoteMask & (1UL << noteSlot)) != 0 &&
                notes_[noteSlot].active &&
                voice.noteToken == notes_[noteSlot].token) {
                beginReleaseLocked(voice);
            }
        }
    }

    portEXIT_CRITICAL(&stateMux_);
}

void AudioSynthWavetableV2::allSoundOff(int channel) {
    if (channel < -1 || channel > 15) return;

    portENTER_CRITICAL(&stateMux_);
    for (uint8_t i = 0; i < MAX_NOTES; ++i) {
        NoteState &state = notes_[i];
        if (state.active && (channel < 0 || state.channel == (uint8_t)channel)) {
            stealNoteLocked(i);
        }
    }
    portEXIT_CRITICAL(&stateMux_);
}

void AudioSynthWavetableV2::sustainPedal(uint8_t channel, bool enabled) {
    if (channel > 15) return;

    portENTER_CRITICAL(&stateMux_);
    const bool wasEnabled = sustain_[channel];
    sustain_[channel] = enabled;

    if (wasEnabled && !enabled) {
        for (uint8_t i = 0; i < MAX_NOTES; ++i) {
            NoteState &state = notes_[i];
            if (state.active && state.channel == channel &&
                !state.keyDown && state.sustainHeld) {
                state.sustainHeld = false;
                releaseNoteLocked(i);
            }
        }
    }
    portEXIT_CRITICAL(&stateMux_);
}

void AudioSynthWavetableV2::amplitude(float value) {
    if (!isfinite(value)) return;
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;

    const uint16_t gain = (uint16_t)(value * UINT16_MAX + 0.5f);
    portENTER_CRITICAL(&stateMux_);
    masterGain_ = gain;
    portEXIT_CRITICAL(&stateMux_);
}

bool AudioSynthWavetableV2::isPlaying() const {
    portENTER_CRITICAL(&stateMux_);
    const bool playing = activeVoiceCount_ != 0;
    portEXIT_CRITICAL(&stateMux_);
    return playing;
}

uint8_t AudioSynthWavetableV2::activeNoteCount() const {
    portENTER_CRITICAL(&stateMux_);
    uint8_t count = 0;
    for (uint8_t i = 0; i < MAX_NOTES; ++i) {
        if (notes_[i].active) ++count;
    }
    portEXIT_CRITICAL(&stateMux_);
    return count;
}

uint16_t AudioSynthWavetableV2::activeSampleCount() const {
    portENTER_CRITICAL(&stateMux_);
    const uint16_t count = activeVoiceCount_;
    portEXIT_CRITICAL(&stateMux_);
    return count;
}

void AudioSynthWavetableV2::renderVoice(SampleVoice &voice, int32_t *mix) const {
    if (!voice.active || voice.sample == nullptr) return;

    const sample_data &sample = *voice.sample;
    const bool stopAtEnd = sample.LOOP_MODE == LoopMode::ONE_SHOT ||
                           (sample.LOOP_MODE == LoopMode::UNTIL_RELEASE &&
                            voice.released);

    // Select the loop policy once per voice. Continuous/held samples with no
    // loop range retain their previous behavior: they neither wrap nor stop.
    if (stopAtEnd) {
        renderVoicePath<true, false, false>(voice, mix, nullptr);
    } else if (sample.LOOP_PHASE_LENGTH != 0) {
        renderVoicePath<false, true, false>(voice, mix, nullptr);
    } else {
        renderVoicePath<false, false, false>(voice, mix, nullptr);
    }
}

void AudioSynthWavetableV2::renderVoiceStereo(SampleVoice &voice,
                                               int32_t *mixLeft,
                                               int32_t *mixRight) const {
    if (!voice.active || voice.sample == nullptr ||
        voice.sample->SAMPLE_RIGHT == nullptr) {
        return;
    }

    const sample_data &sample = *voice.sample;
    const bool stopAtEnd = sample.LOOP_MODE == LoopMode::ONE_SHOT ||
                           (sample.LOOP_MODE == LoopMode::UNTIL_RELEASE &&
                            voice.released);

    if (stopAtEnd) {
        renderVoicePath<true, false, true>(voice, mixLeft, mixRight);
    } else if (sample.LOOP_PHASE_LENGTH != 0) {
        renderVoicePath<false, true, true>(voice, mixLeft, mixRight);
    } else {
        renderVoicePath<false, false, true>(voice, mixLeft, mixRight);
    }
}

template <bool StopAtEnd, bool WrapLoop, bool Stereo>
__attribute__((noinline))
void AudioSynthWavetableV2::renderVoicePath(SampleVoice &voice,
                                             int32_t *mixLeft,
                                             int32_t *mixRight) const {
    const sample_data &sample = *voice.sample;

    const uint32_t maxPhase = maximumPhase(sample);
    const uint32_t loopLength = sample.LOOP_PHASE_LENGTH;
    const uint32_t loopEnd = sample.LOOP_PHASE_END;
    const uint32_t interpolationShift = 16U - sample.INDEX_BITS;
    const uint32_t maxSampleIndex = sample.SAMPLE_COUNT - 2U;

    // Keep the phase accumulator in a register throughout this block. Only
    // store it back once, rather than loading/storing it for every sample.
    uint32_t phase = voice.phase;
    const uint32_t phaseIncrement = voice.phaseIncrement;

    // Release has a fixed linear slope. Its amplitude-scaled ramp was prepared
    // by beginReleaseLocked(), so this path needs only integer subtracts and one
    // sample multiply per frame; the generic ADSR state machine is bypassed.
    if (voice.envelopeState == STATE_RELEASE) {
        for (uint16_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            if (!voice.active) break;

            if (StopAtEnd && phase >= maxPhase) {
                voice.active = false;
                voice.envelopeState = STATE_IDLE;
                break;
            }

            if (voice.envelopeSubCount == 0 && voice.envelopeCount <= 0) {
                voice.active = false;
                voice.envelopeState = STATE_IDLE;
                voice.envelopeCount = 0;
                voice.envelopeMultiplier = 0;
                voice.envelopeIncrement = 0;
                voice.releaseGainQ16_16 = 0;
                voice.releaseGainRemainder = 0;
                voice.releaseGainStepQ16_16 = 0;
                voice.releaseGainStepRemainder = 0;
                voice.phase = phase;
                return;
            }

            // The per-sample release decrement is already scaled by the
            // voice amplitude. Only the fixed-point quotient/remainder needs
            // updating; envelopeMultiplier is no longer used in this state.
            // Unsigned underflow marks the remainder borrow in bit 31. Folding
            // that borrow into the 15-bit remainder and Q16 decrement replaces
            // the per-sample if/else without changing the release ramp.
            const uint32_t remainderDiff =
                (uint32_t)voice.releaseGainRemainder -
                (uint32_t)voice.releaseGainStepRemainder;
            const uint32_t remainderBorrow = remainderDiff >> 31;
            voice.releaseGainRemainder =
                (uint16_t)(remainderDiff + (remainderBorrow << 15));
            voice.releaseGainQ16_16 -=
                voice.releaseGainStepQ16_16 + remainderBorrow;

            ++voice.envelopeSubCount;
            if (voice.envelopeSubCount >= ENVELOPE_PERIOD) {
                voice.envelopeSubCount = 0;
                if (voice.envelopeCount > INT32_MIN) {
                    --voice.envelopeCount;
                }
            }

            const uint32_t phaseFraction = phase >> interpolationShift;
            uint32_t sampleIndex = phaseFraction >> 16;
            if (!StopAtEnd && !WrapLoop && sampleIndex > maxSampleIndex) {
                sampleIndex = maxSampleIndex;
            }

            const uint32_t interpolation = phaseFraction & 0xFFFFU;
            const int32_t sample0 = sample.SAMPLE[sampleIndex];
            const int32_t sample1 = sample.SAMPLE[sampleIndex + 1U];

            const int32_t weight0 = (int32_t)(0x10000U - interpolation);
            const int32_t weightedSample =
                sample0 * weight0 + sample1 * (int32_t)interpolation;
            const int32_t interpolated = weightedSample >> 16;

            const uint32_t effectiveAmplitude =
                voice.releaseGainQ16_16 >> 16;
            mixLeft[i] += ((int32_t)effectiveAmplitude * interpolated) >> 16;
            if (Stereo) {
                const int32_t sample0Right = sample.SAMPLE_RIGHT[sampleIndex];
                const int32_t sample1Right = sample.SAMPLE_RIGHT[sampleIndex + 1U];
                const int32_t weightedSampleRight =
                    sample0Right * weight0 + sample1Right * (int32_t)interpolation;
                const int32_t interpolatedRight = weightedSampleRight >> 16;
                mixRight[i] +=
                    ((int32_t)effectiveAmplitude * interpolatedRight) >> 16;
            }

            phase += phaseIncrement;

            if (WrapLoop && phase >= loopEnd) {
                const uint32_t loopStart = loopEnd - loopLength;
                const uint32_t overshoot = phase - loopEnd;
                phase = loopStart + (overshoot % loopLength);
            }
        }
        voice.phase = phase;
        return;
    }

    for (uint16_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        if (!voice.active) break;

        // A sustained envelope is constant until Note-Off. Render the rest of
        // this block without per-sample envelope bookkeeping or gain setup.
        if (voice.envelopeState == STATE_SUSTAIN) {
            const uint32_t envelopeGainQ16 =
                (uint32_t)voice.envelopeMultiplier >> 15;
            const uint32_t effectiveAmplitude =
                ((uint32_t)voice.amplitude * envelopeGainQ16) >> 16;

            for (; i < AUDIO_BLOCK_SAMPLES; ++i) {
                if (!voice.active) break;

                if (StopAtEnd && phase >= maxPhase) {
                    voice.active = false;
                    voice.envelopeState = STATE_IDLE;
                    break;
                }

                const uint32_t phaseFraction = phase >> interpolationShift;
                uint32_t sampleIndex = phaseFraction >> 16;
                if (!StopAtEnd && !WrapLoop && sampleIndex > maxSampleIndex) {
                    sampleIndex = maxSampleIndex;
                }

                const uint32_t interpolation = phaseFraction & 0xFFFFU;
                const int32_t sample0 = sample.SAMPLE[sampleIndex];
                const int32_t sample1 = sample.SAMPLE[sampleIndex + 1U];

                const int32_t weight0 = (int32_t)(0x10000U - interpolation);
                const int32_t weightedSample =
                    sample0 * weight0 + sample1 * (int32_t)interpolation;
                const int32_t interpolated = weightedSample >> 16;

                mixLeft[i] += ((int32_t)effectiveAmplitude * interpolated) >> 16;
                if (Stereo) {
                    const int32_t sample0Right = sample.SAMPLE_RIGHT[sampleIndex];
                    const int32_t sample1Right = sample.SAMPLE_RIGHT[sampleIndex + 1U];
                    const int32_t weightedSampleRight =
                        sample0Right * weight0 + sample1Right * (int32_t)interpolation;
                    const int32_t interpolatedRight = weightedSampleRight >> 16;
                    mixRight[i] +=
                        ((int32_t)effectiveAmplitude * interpolatedRight) >> 16;
                }

                phase += phaseIncrement;

                if (WrapLoop && phase >= loopEnd) {
                    const uint32_t loopStart = loopEnd - loopLength;
                    const uint32_t overshoot = phase - loopEnd;
                    phase = loopStart + (overshoot % loopLength);
                }
            }
            break;
        }

        if (StopAtEnd && phase >= maxPhase) {
            voice.active = false;
            voice.envelopeState = STATE_IDLE;
            break;
        }

        // Advance the envelope at audio-sample rate. State transitions and
        // envelope-time divisions happen only once per ENVELOPE_PERIOD samples.
        if (voice.envelopeSubCount == 0) {
            while (voice.envelopeCount <= 0) {
                switch (voice.envelopeState) {
                case STATE_DELAY:
                    voice.envelopeState = STATE_ATTACK;
                    voice.envelopeCount = (int32_t)(voice.attackCount ? voice.attackCount : 1U);
                    voice.envelopeIncrement = (int32_t)((int64_t)UNITY_GAIN /
                        ((int64_t)voice.envelopeCount * ENVELOPE_PERIOD));
                    break;

                case STATE_ATTACK:
                    voice.envelopeMultiplier = UNITY_GAIN;
                    voice.envelopeState = STATE_HOLD;
                    voice.envelopeCount = (int32_t)voice.holdCount;
                    voice.envelopeIncrement = 0;
                    break;

                case STATE_HOLD: {
                    voice.envelopeState = STATE_DECAY;
                    voice.envelopeCount = (int32_t)(voice.decayCount ? voice.decayCount : 1U);
                    int32_t sustainLoss = voice.sample->SUSTAIN_MULT;
                    if (sustainLoss < 0) sustainLoss = 0;
                    if (sustainLoss > UNITY_GAIN) sustainLoss = UNITY_GAIN;
                    voice.envelopeIncrement = (int32_t)(-((int64_t)sustainLoss) /
                        ((int64_t)voice.envelopeCount * ENVELOPE_PERIOD));
                    break;
                }

                case STATE_DECAY: {
                    int32_t sustainLoss = voice.sample->SUSTAIN_MULT;
                    if (sustainLoss < 0) sustainLoss = 0;
                    if (sustainLoss > UNITY_GAIN) sustainLoss = UNITY_GAIN;
                    voice.envelopeMultiplier = UNITY_GAIN - sustainLoss;
                    voice.envelopeState = STATE_SUSTAIN;
                    voice.envelopeCount = INT32_MAX;
                    voice.envelopeIncrement = 0;
                    break;
                }

                case STATE_SUSTAIN:
                    voice.envelopeCount = INT32_MAX;
                    voice.envelopeIncrement = 0;
                    break;

                case STATE_RELEASE:
                case STATE_IDLE:
                default:
                    voice.envelopeState = STATE_IDLE;
                    voice.active = false;
                    voice.envelopeCount = 0;
                    voice.envelopeMultiplier = 0;
                    voice.envelopeIncrement = 0;
                    voice.phase = phase;
                    return;
                }

                if (voice.envelopeState == STATE_SUSTAIN) break;
            }
        }

        // The envelope multiplier is nonnegative Q31. Saturating 32-bit
        // arithmetic avoids a 64-bit add in every rendered sample.
        const int32_t envelopeIncrement = voice.envelopeIncrement;
        if (envelopeIncrement > 0) {
            if (voice.envelopeMultiplier >= UNITY_GAIN - envelopeIncrement) {
                voice.envelopeMultiplier = UNITY_GAIN;
            } else {
                voice.envelopeMultiplier += envelopeIncrement;
            }
        } else if (envelopeIncrement < 0) {
            const int32_t zeroThreshold = -envelopeIncrement;
            if (voice.envelopeMultiplier <= zeroThreshold) {
                voice.envelopeMultiplier = 0;
            } else {
                voice.envelopeMultiplier += envelopeIncrement;
            }
        }

        ++voice.envelopeSubCount;
        if (voice.envelopeSubCount >= ENVELOPE_PERIOD) {
            voice.envelopeSubCount = 0;
            if (voice.envelopeState != STATE_SUSTAIN &&
                voice.envelopeCount > INT32_MIN) {
                --voice.envelopeCount;
            }
        }

        const uint32_t phaseFraction = phase >> interpolationShift;
        uint32_t sampleIndex = phaseFraction >> 16;
        if (!StopAtEnd && !WrapLoop && sampleIndex > maxSampleIndex) {
            sampleIndex = maxSampleIndex;
        }

        const uint32_t interpolation = phaseFraction & 0xFFFFU;
        const int32_t sample0 = sample.SAMPLE[sampleIndex];
        const int32_t sample1 = sample.SAMPLE[sampleIndex + 1U];

        const int32_t weight0 = (int32_t)(0x10000U - interpolation);
        const int32_t weightedSample =
            sample0 * weight0 + sample1 * (int32_t)interpolation;
        const int32_t interpolated = weightedSample >> 16;

        // Fold the Q31 envelope into a Q16 voice amplitude. The resulting
        // 16-bit gain has at most one Q16 gain-step of quantization and lets
        // both multiplications below use native 32-bit arithmetic.
        const uint32_t envelopeGainQ16 =
            (uint32_t)voice.envelopeMultiplier >> 15;
        const uint32_t effectiveAmplitude =
            ((uint32_t)voice.amplitude * envelopeGainQ16) >> 16;
        const int32_t value =
            ((int32_t)effectiveAmplitude * interpolated) >> 16;

        mixLeft[i] += value;
        if (Stereo) {
            const int32_t sample0Right = sample.SAMPLE_RIGHT[sampleIndex];
            const int32_t sample1Right = sample.SAMPLE_RIGHT[sampleIndex + 1U];
            const int32_t weightedSampleRight =
                sample0Right * weight0 + sample1Right * (int32_t)interpolation;
            const int32_t interpolatedRight = weightedSampleRight >> 16;
            const int32_t valueRight =
                ((int32_t)effectiveAmplitude * interpolatedRight) >> 16;
            mixRight[i] += valueRight;
        }

        phase += phaseIncrement;

        if (WrapLoop && phase >= loopEnd) {
            const uint32_t loopStart = loopEnd - loopLength;
            const uint32_t overshoot = phase - loopEnd;
            phase = loopStart + (overshoot % loopLength);
        }
    }
    voice.phase = phase;
}

void AudioSynthWavetableV2::updateStereo(uint16_t masterGain) {
    audio_block_t *blockLeft = allocate();
    if (blockLeft == nullptr) return;

    // Stereo needs a second output block. If the pool is temporarily short,
    // keep audio running by producing a mono downmix in the first block.
    audio_block_t *blockRight = allocate();

    alignas(16) int32_t mixLeft[AUDIO_BLOCK_SAMPLES] = {};
    alignas(16) int32_t mixRight[AUDIO_BLOCK_SAMPLES] = {};

    uint32_t noteHasActiveLayer = 0;

    // First render mono voices with the unchanged mono kernel. Their completed
    // mix is duplicated once, before any stereo voice is added.
    for (uint16_t i = 0; i < activeVoiceCount_; ++i) {
        SampleVoice &voice = sampleVoices_[activeVoiceSlots_[i]];
        if (!voice.active) continue;
        if (voice.sample != nullptr &&
            voice.sample->SAMPLE_RIGHT != nullptr) {
            continue;
        }

        renderVoice(voice, mixLeft);

        if (voice.active && voice.noteSlot < MAX_NOTES) {
            const NoteState &noteState = notes_[voice.noteSlot];
            if (noteState.active && noteState.token == voice.noteToken) {
                noteHasActiveLayer |= (1UL << voice.noteSlot);
            }
        }
    }

    memcpy(mixRight, mixLeft, sizeof(mixLeft));

    // Stereo voices share phase, pitch, loop and envelope state. Only the PCM
    // fetch, interpolation and final channel accumulation are performed twice.
    for (uint16_t i = 0; i < activeVoiceCount_; ++i) {
        SampleVoice &voice = sampleVoices_[activeVoiceSlots_[i]];
        if (!voice.active || voice.sample == nullptr ||
            voice.sample->SAMPLE_RIGHT == nullptr) {
            continue;
        }

        renderVoiceStereo(voice, mixLeft, mixRight);

        if (voice.active && voice.noteSlot < MAX_NOTES) {
            const NoteState &noteState = notes_[voice.noteSlot];
            if (noteState.active && noteState.token == voice.noteToken) {
                noteHasActiveLayer |= (1UL << voice.noteSlot);
            }
        }
    }

    compactInactiveVoices();

    for (uint8_t noteSlot = 0; noteSlot < MAX_NOTES; ++noteSlot) {
        if (notes_[noteSlot].active &&
            (noteHasActiveLayer & (1UL << noteSlot)) == 0) {
            notes_[noteSlot] = NoteState{};
        }
    }

    if (blockRight != nullptr) {
#if defined(__riscv) && defined(CONFIG_IDF_TARGET_ESP32P4) && defined(__riscv_xespv)
        uint16_t i = 0;
#if defined(__GNUC__)
#pragma GCC unroll 2
#endif
        for (; (uint16_t)(i + 3U) < AUDIO_BLOCK_SAMPLES; i += 4U) {
            wavetableV2ScaleSaturate4P4(&mixLeft[i], masterGain, &blockLeft->data[i]);
            wavetableV2ScaleSaturate4P4(&mixRight[i], masterGain, &blockRight->data[i]);
        }
        for (; i < AUDIO_BLOCK_SAMPLES; ++i) {
            blockLeft->data[i] =
                wavetableV2Saturate16(wavetableV2ScaleQ16(mixLeft[i], masterGain));
            blockRight->data[i] =
                wavetableV2Saturate16(wavetableV2ScaleQ16(mixRight[i], masterGain));
        }
#else
#if defined(__GNUC__)
#pragma GCC unroll 2
#endif
        for (uint16_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            blockLeft->data[i] =
                wavetableV2Saturate16(wavetableV2ScaleQ16(mixLeft[i], masterGain));
            blockRight->data[i] =
                wavetableV2Saturate16(wavetableV2ScaleQ16(mixRight[i], masterGain));
        }
#endif
        transmit(blockLeft, 0);
        transmit(blockRight, 1);
        release(blockLeft);
        release(blockRight);
    } else {
        // Low-memory fallback: average the stereo mix to mono and mirror the
        // same block to both outputs instead of dropping this audio update.
#if defined(__GNUC__)
#pragma GCC unroll 2
#endif
        for (uint16_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            const int32_t mono = (mixLeft[i] + mixRight[i]) >> 1;
            blockLeft->data[i] =
                wavetableV2Saturate16(wavetableV2ScaleQ16(mono, masterGain));
        }
        transmit(blockLeft, 0);
        transmit(blockLeft, 1);
        release(blockLeft);
    }
}

void AudioSynthWavetableV2::update() {
    // update() runs on the high-priority AudioTask on AUDIO_PROCESSING_CORE.
    // All control calls are made on that same core at lower task priority, so
    // they cannot run concurrently with this renderer. Control-side critical
    // sections prevent AudioTask from preempting multi-field state changes.
    const uint16_t masterGain = masterGain_;

    if (activeVoiceCount_ == 0) return;
    if (activeStereoVoiceCount_ != 0) {
        updateStereo(masterGain);
        return;
    }

    audio_block_t *block = allocate();
    if (block == nullptr) return;

    int32_t mix[AUDIO_BLOCK_SAMPLES] = {};

    // Render only the dense active-slot list. Build the active-note bitmap
    // after each voice has been rendered, so voices that end during this block
    // are not counted as active layers. Ended slots are removed in-place.
    uint32_t noteHasActiveLayer = 0;
    for (uint16_t i = 0; i < activeVoiceCount_;) {
        const uint16_t slot = activeVoiceSlots_[i];
        SampleVoice &voice = sampleVoices_[slot];

        renderVoice(voice, mix);

        if (voice.active) {
            if (voice.noteSlot < MAX_NOTES) {
                const NoteState &noteState = notes_[voice.noteSlot];
                if (noteState.active && noteState.token == voice.noteToken) {
                    noteHasActiveLayer |= (1UL << voice.noteSlot);
                }
            }
            ++i;
        } else {
            activeVoiceSlots_[i] = activeVoiceSlots_[--activeVoiceCount_];
        }
    }

    // Retire notes whose final sample layer has ended.

    for (uint8_t noteSlot = 0; noteSlot < MAX_NOTES; ++noteSlot) {
        if (notes_[noteSlot].active &&
            (noteHasActiveLayer & (1UL << noteSlot)) == 0) {
            notes_[noteSlot] = NoteState{};
        }
    }

    // Apply Q16 master gain and saturate to signed 16-bit PCM.
#if defined(__riscv) && defined(CONFIG_IDF_TARGET_ESP32P4) && defined(__riscv_xespv)
    uint16_t i = 0;
#if defined(__GNUC__)
#pragma GCC unroll 2
#endif
    for (; (uint16_t)(i + 3U) < AUDIO_BLOCK_SAMPLES; i += 4U) {
        wavetableV2ScaleSaturate4P4(&mix[i], masterGain, &block->data[i]);
    }
    for (; i < AUDIO_BLOCK_SAMPLES; ++i) {
        const int32_t scaled = wavetableV2ScaleQ16(mix[i], masterGain);
        block->data[i] = wavetableV2Saturate16(scaled);
    }
#else
    // Xtensa uses a single CLAMPS instruction for saturation; other targets
    // use the portable comparison-based fallback.
#if defined(__GNUC__)
#pragma GCC unroll 2
#endif
    for (uint16_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        const int32_t scaled = wavetableV2ScaleQ16(mix[i], masterGain);
        block->data[i] = wavetableV2Saturate16(scaled);
    }
#endif

    transmit(block, 0);
    // A mono mix is identical on both outputs and uses the same audio block.
    transmit(block, 1);
    release(block);
}

/*
 * ============================================================================
 * PERFORMANCE NOTES / OPTIMIZATION HISTORY
 * ============================================================================
 *
 * Keep this section current whenever the renderer or its data structures are
 * optimized. It is deliberately kept in this .cpp file so that performance
 * decisions and their rationale remain next to the implementation.
 *
 * IMPORTANT STATUS:
 * - MAX_NOTES * MAX_SAMPLE_LAYERS_PER_NOTE is a DESIGN CAPACITY of 32 * 4 =
 *   128 sample voices. This is not a measured or achieved performance result.
 * - The 128-voice target has NOT yet been reached in the user's hardware tests.
 *   Do not describe it as supported at full real-time load until a complete
 *   stress run proves it on the target board(s).
 * - The active-slot-list change is implemented, but has not yet been validated
 *   by comparative hardware measurements. Treat its performance benefit as
 *   an assumption until baseline/current measurements exist.
 *
 * 1. VOICE STORAGE, ALLOCATION AND UPDATE TRAVERSAL
 * -----------------------------------------------
 * - Fixed-size storage: 32 MIDI NoteState entries and 128 SampleVoice entries.
 *   Each note may start up to four matching sample layers. No dynamic
 *   allocation is required by the audio renderer.
 * - activeVoiceSlots_ is a dense list of indices into sampleVoices_. update()
 *   traverses active entries rather than scanning all 128 SampleVoice slots.
 *   Finished entries are removed by swap-with-last in the mono path; the
 *   stereo path compacts the active list after its two rendering passes.
 * - activeStereoVoiceCount_ selects the mono-only update path when no stereo
 *   voice is active. This avoids entering the stereo renderer for mono-only
 *   instruments. The counter is maintained on voice start, note stealing,
 *   instrument reset and stereo-list compaction; preserve this invariant.
 * - noteOn() still searches the fixed sampleVoices_ pool for free slots on the
 *   control side. The active list optimizes audio traversal, not allocation.
 * - activeVoiceSlots_ costs 256 bytes for 128 uint16_t indices, plus two
 *   counters. The list adds an indirection; at very high polyphony it may not
 *   be faster than a linear contiguous scan. Benchmark before retaining or
 *   changing this design on performance grounds.
 *
 * 2. AUDIO TASK / CONTROL TASK SYNCHRONIZATION
 * -------------------------------------------
 * - The renderer accesses live NoteState and SampleVoice objects directly.
 *   AudioTask is pinned to AUDIO_PROCESSING_CORE and has higher priority than
 *   the control calls, which are expected to run on that same core.
 * - stateMux_ protects multi-field control-side changes against AudioTask
 *   preemption. There are no per-block snapshots, batch copies, generation
 *   checks or stateMux_ lock/unlock operations in the render loop.
 * - The old snapshot/batch-of-eight approach was deliberately replaced by
 *   direct live-state rendering. Do not reintroduce it without revisiting and
 *   measuring the scheduling and synchronization model.
 * - This synchronization assumption is not safe for arbitrary cross-core or
 *   ISR calls. Such use requires a separate synchronization review.
 *
 * 3. COMPILE-TIME SPECIALIZED SAMPLE RENDERER
 * ------------------------------------------
 * - renderVoicePath<StopAtEnd, WrapLoop, Stereo>() specializes the hot loop at
 *   compile time for one-shot/end handling, loop wrapping and mono/stereo.
 *   Those policies are not tested as runtime branches for every PCM sample.
 * - Mono and stereo use distinct template specializations. The mono kernel
 *   contains no per-sample right-channel fetch, interpolation or accumulation.
 * - renderVoicePath() is noinline intentionally to control code size and
 *   register pressure from its template specializations. Do not remove this
 *   based only on source-level intuition; compare generated code and timings.
 * - Voice/sample invariants, including phase increment, loop bounds, sample
 *   pointers and interpolation configuration, are prepared outside the
 *   per-sample work where possible.
 * - Phase is accumulated in a local variable during a block and written back
 *   to voice.phase once at block exit (or at an early return).
 * - PCM uses linear interpolation between adjacent samples. Each sample array
 *   must include the interpolation guard frame described by sample_data.
 *
 * 4. ADSR / ENVELOPE HOT-PATH OPTIMIZATIONS
 * ----------------------------------------
 * - Envelope time is advanced at ENVELOPE_PERIOD granularity (currently
 *   eight audio samples). State transitions and envelope-time divisions are
 *   therefore not repeated for every output sample.
 * - STATE_SUSTAIN has a dedicated inner loop: its gain is constant until
 *   Note-Off, so normal ADSR bookkeeping is skipped while sustaining.
 * - beginReleaseLocked() precomputes an amplitude-scaled release ramp as a
 *   Q16.16 quotient plus a /32768 remainder. The release loop updates this
 *   with 32-bit integer arithmetic and a borrow; no per-sample 64-bit
 *   multiply/divide is needed.
 * - Normal envelope advancement uses bounded/saturating 32-bit arithmetic.
 *   Q31 envelope gain is folded into a Q16 effective voice amplitude before
 *   the PCM multiply, avoiding repeated 64-bit gain multiplies in the loop.
 *
 * 5. MIXING AND MASTER-GAIN / SATURATION
 * -------------------------------------
 * - Voices accumulate into 32-bit mix buffers. Clipping is deferred until
 *   after all active voices have contributed; there is no per-voice 16-bit
 *   saturation.
 * - wavetableV2ScaleQ16() implements the Q16 master-gain multiply by splitting
 *   the signed input into high and low 16-bit parts. It avoids a 64-bit
 *   multiply in the final per-sample conversion while preserving the intended
 *   fixed-point result.
 * - The mono path allocates one output block, then transmits that same block
 *   to left and right. It does not allocate or calculate a duplicate mono mix.
 *
 * 6. STEREO PATH
 * -------------
 * - A stereo sample stores linked left/right PCM arrays but shares one voice
 *   state: phase, pitch, loop position and envelope. The state machine is not
 *   advanced twice for the two channels.
 * - In updateStereo(), mono voices use the mono renderer first; their mix is
 *   copied to the right accumulator once. Stereo voices then add their
 *   independently interpolated left and right samples.
 * - Mono-only playback dispatches to update(), not updateStereo(). In a mixed
 *   mono/stereo block, the active list is traversed for the mono pass and the
 *   stereo pass; this traversal overhead is worth measuring separately.
 * - If a second audio block cannot be allocated, the stereo mix is averaged
 *   to mono and the one resulting block is mirrored to both outputs. This is
 *   a continuity fallback, not full stereo output.
 *
 * 7. TARGET-SPECIFIC FINAL PCM OPTIMIZATIONS
 * -----------------------------------------
 * - ESP32-S3 / Xtensa LX7: wavetableV2Saturate16() uses CLAMPS when the core
 *   advertises XCHAL_HAVE_CLAMPS; otherwise it uses the portable scalar clamp.
 * - ESP32-P4 / XespV: wavetableV2ScaleSaturate4P4() scales four values with
 *   the Q16 helper, then uses XespV vector instructions for parallel signed
 *   32-bit clamping and 16-bit extraction. A scalar tail and portable fallback
 *   remain. This optimizes final scaling/saturation; it does not vectorize the
 *   per-voice sample renderer.
 * - These target paths must continue to produce bit-identical PCM to the
 *   scalar reference. Verify with builds and comparison tests after changes.
 *
 * 8. SAMPLE-RATE HANDLING
 * ----------------------
 * - setInstrument() reads the active AudioStream sample rate and derives
 *   sampleCountScale_ and phaseIncrementScale_. Envelope/sample durations are
 *   scaled with output rate and phase increments are inversely scaled so that
 *   pitch and time remain consistent when using supported runtime rates such
 *   as 44.1 kHz and 48 kHz.
 * - Call setInstrument() after AudioMemory() has established the intended
 *   sample rate, and call it again after a global sample-rate change.
 *
 * 9. VALIDATION / KNOWN LIMITS / NEXT MEASUREMENTS
 * -----------------------------------------------
 * MEASURED RUN — 2026-11-02 (user-provided serial log):
 * - 48 kHz, USB Audio output, 32 notes / 128 sample layers were successfully
 *   started. The log confirms active_notes=32 and active_layers=128.
 * - Sustain at 128 active layers: node_us=2204, node_peak_us=2387,
 *   graph_us=2316, graph_peak_us=2391; reported V2 CPU was about 82.6–82.8%
 *   with a peak of 89.5%. The graph peak was 89.7%.
 * - Audio block size is 128 samples, so one block at 48 kHz is about 2667 us.
 *   The measured sustain peak (2387 us) is close to this deadline and leaves
 *   limited headroom for other nodes, system activity or less cache-friendly
 *   sample data.
 * - Release node_peak_us by sample-voice count was: 4=204 us, 8=313 us,
 *   16=556 us, 32=1053 us, 64=2072 us and 128=4026 us. At 128 voices,
 *   graph_peak_us was 4030 us (about 151%). The corresponding 128-voice
 *   sustain peak was 2387 us. The release path is therefore substantially
 *   more expensive than sustain, and its peak scales roughly with voice count.
 *   This points toward the per-sample STATE_RELEASE render path as a likely
 *   optimization target, but does not prove the exact cause. The ordinary
 *   sampled load was already zero by the next periodic report because the
 *   release stage had completed. A repeat / isolated release test is needed.
 * - The exact board model is not printed in the log. USB Audio at 48 kHz
 *   suggests the S3 USB test environment, but the hardware identity is
 *   unconfirmed by the log itself.
 * - The stress instrument uses four short 33-frame looping test waveforms,
 *   not representative long SF2 samples. This verifies voice scheduling and
 *   hot-loop work for this workload, but does not yet prove equal performance
 *   with large sample arrays in PSRAM or arbitrary SF2 instruments.
 * - User-provided Audacity capture "test_usb_stereo.wav" was inspected:
 *   48 kHz, 16-bit PCM, 1 channel, 32.38 s. It contains the full note-count
 *   sweep through 128 voices and the beginning of the next sweep repetition.
 *   The 128-voice sustain region contains no silent 128-sample audio blocks
 *   in the inspected interval and no digital full-scale clipping; the maximum
 *   absolute PCM sample was about 0.429. Listening did not reveal an obvious
 *   sustained dropout or click. This is a qualitative observation, not proof
 *   of glitch-free playback under all conditions.
 * - The capture is mono. It therefore cannot verify left/right channel identity
 *   or stereo separation. The mono-only test is expected to produce the same
 *   mono mix on both output channels, but that duplication was not measurable
 *   from this one-channel export.
 * - No I2S result, ESP32-P4 result or repeated-run variance has been recorded.
 *   variance has been recorded yet.
 * - WAVETABLE_V2_TEST_NOTE_COUNT defaults to 1; the full 1/2/4/8/16/32 note
 *   sweep must be explicitly enabled with
 *   -DWAVETABLE_V2_TEST_NOTE_COUNT=32. Four layers per note then request up to
 *   128 sample voices. Record active_layers as the actual number of live
 *   sample voices.
 * - WAVETABLE_V2_TEST_STEREO=0 tests mono-only playback. Setting it to 1 makes
 *   the fourth of the four layers stereo, exercising a mixed mono/stereo load.
 *
 * - setInstrument() validates zone ranges, sample pointers and dimensions,
 *   phase capacity and loop bounds before playback.
 * - noteOn() resolves matching zones and computes per-layer phase increments
 *   outside the control critical section, then installs the voices under the
 *   lock. It starts at most four layers per note.
 * - The stress example is examples/Synthesis/WavetableV2/WavetableV2.ino.
 *   Its default WAVETABLE_V2_TEST_NOTE_COUNT is 1; the full 1/2/4/8/16/32
 *   note sweep must be explicitly enabled with
 *   -DWAVETABLE_V2_TEST_NOTE_COUNT=32. Four layers per note then request up to
 *   128 sample voices. The default example setting does NOT test that target.
 * - WAVETABLE_V2_TEST_STEREO=0 tests mono-only playback. Setting it to 1 makes
 *   the fourth of the four layers stereo, exercising a mixed mono/stereo load.
 * - No 128-voice result, CPU percentage, real-time margin or cross-board
 *   performance comparison is claimed here until the user supplies hardware
 *   logs. Record active_layers as well as requested_notes: the former is the
 *   actual number of live sample voices.
 *
 * 10. BULK NOTE-OFF
 * -----------------
 * - allNotesOff() first updates the matching NoteState entries and records
 *   the note slots that need release in a 32-bit bitmap. Notes held by the
 *   sustain pedal are excluded, preserving channel-specific sustain behavior.
 * - It then scans activeVoiceSlots_ once and calls beginReleaseLocked() for
 *   matching voices whose note token still matches the owning NoteState.
 *   This changes the scan complexity from O(MAX_NOTES * activeVoiceCount_)
 *   to O(MAX_NOTES + activeVoiceCount_) for a bulk note-off.
 * - Release-envelope initialization still runs once per released sample voice
 *   and remains inside stateMux_. This optimization removes redundant list
 *   traversal; it does not optimize the release render loop or its arithmetic.
 * - Implementation committed on 2026-11-02. Hardware performance impact has
 *   not yet been measured; validate with the 32-note / 128-layer benchmark.
 *
 * MAINTENANCE RULE:
 * When changing a hot path, update the relevant item above in the same change.
 * State whether the optimization is implemented, experimentally measured,
 * rejected or pending. Never turn a theoretical capacity or an assumption
 * into a measured result without benchmark evidence.
 * ============================================================================
 */
