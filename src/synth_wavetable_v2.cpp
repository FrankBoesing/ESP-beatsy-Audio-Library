/*
 * ESP-beatsy Audio Library
 *
 * Polyphonic, zone-based sample wavetable synthesizer.
 */

#include "synth_wavetable_v2.h"

#include <limits.h>
#include <math.h>

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
    noteState.layerCount = layerCount;
    noteState.token = token;
    noteState.age = ++nextAge_;

    const uint32_t velocityGain = ((uint32_t)velocity * UINT16_MAX + 63U) / 127U;

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

        if (voice.phaseIncrement == 0) {
            voice.active = false;
            continue;
        }

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

    portEXIT_CRITICAL(&stateMux_);
    return layerCount;
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

void AudioSynthWavetableV2::releaseNoteLocked(uint8_t noteSlot) {
    if (noteSlot >= MAX_NOTES || !notes_[noteSlot].active) return;

    const uint32_t token = notes_[noteSlot].token;
    for (uint16_t i = 0; i < MAX_SAMPLE_VOICES; ++i) {
        SampleVoice &voice = sampleVoices_[i];
        if (voice.active && voice.noteSlot == noteSlot &&
            voice.noteToken == token) {
            beginReleaseLocked(voice);
        }
    }
}

void AudioSynthWavetableV2::stealNoteLocked(uint8_t noteSlot) {
    if (noteSlot >= MAX_NOTES || !notes_[noteSlot].active) return;

    const uint32_t token = notes_[noteSlot].token;
    for (uint16_t i = 0; i < MAX_SAMPLE_VOICES; ++i) {
        SampleVoice &voice = sampleVoices_[i];
        if (voice.active && voice.noteSlot == noteSlot &&
            voice.noteToken == token) {
            voice.active = false;
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
            releaseNoteLocked(i);
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
    bool playing = false;
    for (uint16_t i = 0; i < MAX_SAMPLE_VOICES; ++i) {
        if (sampleVoices_[i].active) {
            playing = true;
            break;
        }
    }
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
    uint16_t count = 0;
    for (uint16_t i = 0; i < MAX_SAMPLE_VOICES; ++i) {
        if (sampleVoices_[i].active) ++count;
    }
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
        renderVoicePath<true, false>(voice, mix);
    } else if (sample.LOOP_PHASE_LENGTH != 0) {
        renderVoicePath<false, true>(voice, mix);
    } else {
        renderVoicePath<false, false>(voice, mix);
    }
}

template <bool StopAtEnd, bool WrapLoop>
void AudioSynthWavetableV2::renderVoicePath(SampleVoice &voice, int32_t *mix) const __attribute__((noinline)) {
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
            mix[i] += ((int32_t)effectiveAmplitude * interpolated) >> 16;

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

                mix[i] += ((int32_t)effectiveAmplitude * interpolated) >> 16;

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

        mix[i] += value;

        phase += phaseIncrement;

        if (WrapLoop && phase >= loopEnd) {
            const uint32_t loopStart = loopEnd - loopLength;
            const uint32_t overshoot = phase - loopEnd;
            phase = loopStart + (overshoot % loopLength);
        }
    }
    voice.phase = phase;
}

void AudioSynthWavetableV2::update() {
    // update() runs on the high-priority AudioTask on AUDIO_PROCESSING_CORE.
    // All control calls are made on that same core at lower task priority, so
    // they cannot run concurrently with this renderer. Control-side critical
    // sections prevent AudioTask from preempting multi-field state changes.
    const uint16_t masterGain = masterGain_;

    bool anyActive = false;
    for (uint16_t i = 0; i < MAX_SAMPLE_VOICES; ++i) {
        if (sampleVoices_[i].active) {
            anyActive = true;
            break;
        }
    }
    if (!anyActive) return;

    audio_block_t *block = allocate();
    if (block == nullptr) return;

    int32_t mix[AUDIO_BLOCK_SAMPLES] = {};

    // Render directly into the live voice array. Build the active-note
    // bitmap after each voice has been rendered, so voices that end during
    // this block are not counted as active layers. This replaces a second
    // full scan over sampleVoices_.
    uint32_t noteHasActiveLayer = 0;
    for (uint16_t i = 0; i < MAX_SAMPLE_VOICES; ++i) {
        SampleVoice &voice = sampleVoices_[i];
        if (!voice.active) continue;

        renderVoice(voice, mix);

        if (voice.active && voice.noteSlot < MAX_NOTES) {
            const NoteState &noteState = notes_[voice.noteSlot];
            if (noteState.active && noteState.token == voice.noteToken) {
                noteHasActiveLayer |= (1UL << voice.noteSlot);
            }
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

    transmit(block);
    release(block);
}
