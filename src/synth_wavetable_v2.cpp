/*
 * ESP-beatsy Audio Library
 *
 * Polyphonic, zone-based sample wavetable synthesizer.
 */

#include "synth_wavetable_v2.h"

#include <limits.h>
#include <math.h>
#include <string.h>

static inline int16_t wavetableV2Saturate16(int32_t value) {
    if (value > 32767) return 32767;
    if (value < -32768) return -32768;
    return (int16_t)value;
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
        const uint32_t generation = sampleVoices_[i].generation + 1U;
        sampleVoices_[i] = SampleVoice{};
        sampleVoices_[i].generation = generation;
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
    return 440.0f * powf(2.0f, ((float)note - 69.0f) / 12.0f);
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

    // Read the immutable instrument descriptor under the lock, then resolve
    // zones outside the critical section. setInstrument() is checked again
    // before any voice slots are changed.
    const instrument_data *instrument;
    portENTER_CRITICAL(&stateMux_);
    instrument = instrument_;
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

    portENTER_CRITICAL(&stateMux_);
    if (instrument_ != instrument) {
        portEXIT_CRITICAL(&stateMux_);
        return 0;
    }
    const float phaseScale = phaseIncrementScale_;

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
    const float frequency = noteToFreq(note);

    for (uint8_t layer = 0; layer < layerCount; ++layer) {
        const uint16_t slot = freeSlots[layer];
        SampleVoice &voice = sampleVoices_[slot];
        const uint32_t generation = voice.generation + 1U;
        const sample_data *sample = matchingSamples[layer];

        voice = SampleVoice{};
        voice.active = true;
        voice.released = false;
        voice.noteSlot = (uint8_t)noteSlot;
        voice.noteToken = token;
        voice.generation = generation;
        voice.sample = sample;
        voice.phase = 0;

        float increment = frequency * sample->PER_HERTZ_PHASE_INCREMENT * phaseScale;
        if (!isfinite(increment) || increment <= 0.0f) {
            voice.active = false;
            continue;
        }
        if (increment < 1.0f) increment = 1.0f;
        voice.phaseIncrement = increment >= 4294967295.0f
                                   ? UINT32_MAX
                                   : (uint32_t)(increment + 0.5f);

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
        ++voice.generation;
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
    ++voice.generation;
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
            ++voice.generation;
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
    const bool loopEnabled = sample.LOOP_MODE == LoopMode::CONTINUOUS ||
                             (sample.LOOP_MODE == LoopMode::UNTIL_RELEASE &&
                              !voice.released);

    const uint32_t maxPhase = maximumPhase(sample);
    const uint32_t loopLength = sample.LOOP_PHASE_LENGTH;
    const uint32_t loopEnd = sample.LOOP_PHASE_END;
    const uint32_t sampleIndexShift = 32U - sample.INDEX_BITS;
    const uint32_t interpolationShift = 16U - sample.INDEX_BITS;
    const uint32_t maxSampleIndex = sample.SAMPLE_COUNT - 2U;

    for (uint16_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        if (stopAtEnd && voice.phase >= maxPhase) {
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

        uint32_t sampleIndex = voice.phase >> sampleIndexShift;
        if (sampleIndex > maxSampleIndex) {
            sampleIndex = maxSampleIndex;
        }

        const uint32_t interpolation =
            (voice.phase >> interpolationShift) & 0xFFFFU;
        const int32_t sample0 = sample.SAMPLE[sampleIndex];
        const int32_t sample1 = sample.SAMPLE[sampleIndex + 1U];

        // Linear interpolation with weights summing to exactly 1.0 in Q16.
        // The weighted sum remains in int32_t because it is a convex mix of
        // two signed 16-bit samples.
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

        voice.phase += voice.phaseIncrement;

        if (loopEnabled && loopLength != 0 && voice.phase >= loopEnd) {
            const uint32_t loopStart = loopEnd - loopLength;
            const uint32_t overshoot = voice.phase - loopEnd;
            voice.phase = loopStart + (overshoot % loopLength);
        }
    }
}

void AudioSynthWavetableV2::update() {
    bool anyActive = false;
    uint16_t masterGain;

    portENTER_CRITICAL(&stateMux_);
    masterGain = masterGain_;
    for (uint16_t i = 0; i < MAX_SAMPLE_VOICES; ++i) {
        if (sampleVoices_[i].active) {
            anyActive = true;
            break;
        }
    }
    portEXIT_CRITICAL(&stateMux_);

    if (!anyActive) return;

    audio_block_t *block = allocate();
    if (block == nullptr) return;

    int32_t mix[AUDIO_BLOCK_SAMPLES] = {};

    // Copy and commit small batches rather than locking separately for every
    // sample voice. Eight voices keep the AudioTask stack use bounded while
    // reducing the lock/unlock count by a factor of eight.
    constexpr uint16_t VOICE_BATCH_SIZE = 8;
    SampleVoice localVoices[VOICE_BATCH_SIZE];

    for (uint16_t base = 0; base < MAX_SAMPLE_VOICES; base += VOICE_BATCH_SIZE) {
        uint16_t batchCount = MAX_SAMPLE_VOICES - base;
        if (batchCount > VOICE_BATCH_SIZE) {
            batchCount = VOICE_BATCH_SIZE;
        }

        portENTER_CRITICAL(&stateMux_);
        for (uint16_t i = 0; i < batchCount; ++i) {
            localVoices[i] = sampleVoices_[base + i];
        }
        portEXIT_CRITICAL(&stateMux_);

        for (uint16_t i = 0; i < batchCount; ++i) {
            if (localVoices[i].active) {
                renderVoice(localVoices[i], mix);
            }
        }

        // MIDI/control operations may alter a voice while its local snapshot is
        // rendered. The generation check prevents committing stale state over
        // a newer noteOn(), noteOff(), allSoundOff() or instrument change.
        portENTER_CRITICAL(&stateMux_);
        for (uint16_t i = 0; i < batchCount; ++i) {
            SampleVoice &current = sampleVoices_[base + i];
            if (current.generation == localVoices[i].generation) {
                current = localVoices[i];
            }
        }

        // On the last batch, also retire notes whose sample layers have ended.
        // This replaces the previous notes x voices nested scan with one scan
        // through the active voices, inside the same critical section.
        if (base + batchCount == MAX_SAMPLE_VOICES) {
            bool noteHasActiveLayer[MAX_NOTES] = {};

            for (uint16_t i = 0; i < MAX_SAMPLE_VOICES; ++i) {
                const SampleVoice &voice = sampleVoices_[i];
                if (!voice.active || voice.noteSlot >= MAX_NOTES) continue;

                const NoteState &noteState = notes_[voice.noteSlot];
                if (noteState.active && noteState.token == voice.noteToken) {
                    noteHasActiveLayer[voice.noteSlot] = true;
                }
            }

            for (uint8_t noteSlot = 0; noteSlot < MAX_NOTES; ++noteSlot) {
                if (notes_[noteSlot].active && !noteHasActiveLayer[noteSlot]) {
                    notes_[noteSlot] = NoteState{};
                }
            }
        }
        portEXIT_CRITICAL(&stateMux_);
    }

    for (uint16_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        const int32_t scaled = (int32_t)(((int64_t)mix[i] * masterGain) >> 16);
        block->data[i] = wavetableV2Saturate16(scaled);
    }

    transmit(block);
    release(block);
}
