/*
 * Audio Library for Teensy 3.X
 * Copyright (c) 2017, TeensyAudio PSU Team
 *
 * Development of this audio library was sponsored by PJRC.COM, LLC.
 * Please support PJRC's efforts to develop open source software by purchasing
 * Teensy or other PJRC products.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice, development funding notice, and this permission
 * notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "synth_wavetable.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "utility/dspinst.h"

// Portable equivalents of the ARM DSP operations used by the original source.
static inline int32_t wavetable_mul32x32_rshift32(int32_t a, int32_t b) {
    return (int32_t)(((int64_t)a * (int64_t)b) >> 32);
}

static inline int32_t wavetable_mul32x32_rshift32_rounded(int32_t a, int32_t b) {
    return (int32_t)((((int64_t)a * (int64_t)b) + INT64_C(0x80000000)) >> 32);
}

static inline int32_t wavetable_macc32x32_rshift32_rounded(
    int32_t sum, int32_t a, int32_t b) {
    return sum + wavetable_mul32x32_rshift32_rounded(a, b);
}

static inline int32_t wavetable_macc32x16b(int32_t sum, int32_t a, uint32_t b) {
    return sum + (int32_t)(((int64_t)a * (int16_t)(b & 0xFFFFU)) >> 16);
}

static inline int32_t wavetable_mul32x16b(int32_t a, int16_t b) {
    return (int32_t)(((int64_t)a * (int32_t)b) >> 16);
}

static inline int16_t wavetable_saturate16(int32_t value) {
    if (value > INT16_MAX) {
        return INT16_MAX;
    }
    if (value < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)value;
}

static inline int32_t wavetable_clamp_envelope(int64_t value) {
    if (value > AudioSynthWavetable::UNITY_GAIN) {
        return AudioSynthWavetable::UNITY_GAIN;
    }
    if (value < 0) {
        return 0;
    }
    return (int32_t)value;
}

void AudioSynthWavetable::setInstrument(const instrument_data &newInstrument) {
    portENTER_CRITICAL(&stateMux);
    instrument = &newInstrument;
    current_sample = nullptr;
    env_state = STATE_IDLE;
    tone_phase = 0;
    env_count = 0;
    env_mult = 0;
    env_incr = 0;
    vib_count = mod_count = 0;
    vib_phase = mod_phase = (uint32_t)TRIANGLE_INITIAL_PHASE;
    ++stateGeneration;
    portEXIT_CRITICAL(&stateMux);
}

void AudioSynthWavetable::amplitude(float value) {
    if (!isfinite(value)) {
        return;
    }
    if (value < 0.0f) {
        value = 0.0f;
    } else if (value > 1.0f) {
        value = 1.0f;
    }

    const uint16_t newAmplitude = (uint16_t)(UINT16_MAX * value);
    portENTER_CRITICAL(&stateMux);
    tone_amp = newAmplitude;
    portEXIT_CRITICAL(&stateMux);
}

float AudioSynthWavetable::midi_volume_transform(int midi_amp) {
    if (midi_amp < 0) {
        midi_amp = 0;
    } else if (midi_amp > 127) {
        midi_amp = 127;
    }
    const float normalized = (float)midi_amp / 127.0f;
    return normalized * normalized * normalized * normalized;
}

float AudioSynthWavetable::noteToFreq(int note) {
    if (note < 0) {
        note = 0;
    } else if (note > 127) {
        note = 127;
    }
    const float exponent = (float)note * (1.0f / 12.0f) + 3.0313597f;
    return powf(2.0f, exponent);
}

int AudioSynthWavetable::freqToNote(float freq) {
    if (!isfinite(freq) || freq <= 0.0f) {
        return 0;
    }
    const float note = 12.0f * log2f(freq) - 35.8763164f;
    if (note <= 0.0f) {
        return 0;
    }
    if (note >= 127.0f) {
        return 127;
    }
    return (int)note;
}

void AudioSynthWavetable::stop() {
    portENTER_CRITICAL(&stateMux);
    if (env_state != STATE_IDLE) {
        env_state = STATE_RELEASE;
        env_count = current_sample != nullptr
                        ? (int32_t)current_sample->RELEASE_COUNT
                        : 1;
        if (env_count <= 0) {
            env_count = 1;
        }
        const int64_t denominator = (int64_t)env_count * ENVELOPE_PERIOD;
        env_incr = denominator > 0 ? (int32_t)(-(int64_t)env_mult / denominator) : 0;
        ++stateGeneration;
    }
    portEXIT_CRITICAL(&stateMux);
}

void AudioSynthWavetable::playFrequency(float freq, int amp) {
    if (!isfinite(freq)) {
        return;
    }
    if (freq < 1.0f) {
        freq = 1.0f;
    } else if (freq > AUDIO_SAMPLE_RATE_EXACT * 0.5f) {
        freq = AUDIO_SAMPLE_RATE_EXACT * 0.5f;
    }

    setState(freqToNote(freq), amp, freq);
}

void AudioSynthWavetable::playNote(int note, int amp) {
    if (note < 0) {
        note = 0;
    } else if (note > 127) {
        note = 127;
    }
    setState(note, amp, noteToFreq(note));
}

void AudioSynthWavetable::setFrequencyLocked(float freq) {
    if (current_sample == nullptr || !isfinite(freq) || freq < 0.0f) {
        return;
    }

    const float increment = freq * current_sample->PER_HERTZ_PHASE_INCREMENT;
    if (increment >= 4294967295.0f) {
        tone_incr = UINT32_MAX;
    } else if (increment <= 0.0f) {
        tone_incr = 0;
    } else {
        tone_incr = (uint32_t)increment;
    }

    vib_pitch_offset_init = (int32_t)(
        increment * current_sample->VIBRATO_PITCH_COEFFICIENT_INITIAL);
    vib_pitch_offset_scnd = (int32_t)(
        increment * current_sample->VIBRATO_PITCH_COEFFICIENT_SECOND);
    mod_pitch_offset_init = (int32_t)(
        increment * current_sample->MODULATION_PITCH_COEFFICIENT_INITIAL);
    mod_pitch_offset_scnd = (int32_t)(
        increment * current_sample->MODULATION_PITCH_COEFFICIENT_SECOND);
}

void AudioSynthWavetable::setFrequency(float freq) {
    if (!isfinite(freq) || freq < 0.0f) {
        return;
    }

    portENTER_CRITICAL(&stateMux);
    if (current_sample != nullptr) {
        setFrequencyLocked(freq);
        ++stateGeneration;
    }
    portEXIT_CRITICAL(&stateMux);
}

void AudioSynthWavetable::setState(int note, int amp, float freq) {
    if (!isfinite(freq) || freq <= 0.0f) {
        return;
    }
    if (amp < 0) {
        amp = 0;
    } else if (amp > 127) {
        amp = 127;
    }

    portENTER_CRITICAL(&stateMux);

    env_state = STATE_IDLE;
    current_sample = nullptr;

    if (instrument != nullptr && instrument->sample_count != 0 &&
        instrument->sample_note_ranges != nullptr && instrument->samples != nullptr) {
        uint8_t index = 0;
        while ((uint16_t)(index + 1U) < instrument->sample_count &&
               note > instrument->sample_note_ranges[index]) {
            ++index;
        }

        const sample_data *candidate = &instrument->samples[index];
        if (candidate->sample != nullptr &&
            candidate->INDEX_BITS > 0 && candidate->INDEX_BITS <= 16) {
            current_sample = candidate;
        }
    }

    if (current_sample != nullptr) {
        setFrequencyLocked(freq);
        vib_count = mod_count = tone_phase = env_incr = env_mult = 0;
        vib_phase = mod_phase = (uint32_t)TRIANGLE_INITIAL_PHASE;
        env_count = (int32_t)current_sample->DELAY_COUNT;

        tone_amp = (uint16_t)(amp * (UINT16_MAX / 127));
        tone_amp = (uint16_t)(((uint32_t)current_sample->INITIAL_ATTENUATION_SCALAR *
                               (uint32_t)tone_amp) >> 16);

        env_state = STATE_DELAY;
    } else {
        env_count = 0;
        env_mult = 0;
        env_incr = 0;
    }

    ++stateGeneration;
    portEXIT_CRITICAL(&stateMux);
}

bool AudioSynthWavetable::isPlaying() {
    portENTER_CRITICAL(&stateMux);
    const bool playing = env_state != STATE_IDLE;
    portEXIT_CRITICAL(&stateMux);
    return playing;
}

AudioSynthWavetable::envelopeStateEnum AudioSynthWavetable::getEnvState() {
    portENTER_CRITICAL(&stateMux);
    const envelopeStateEnum state = env_state;
    portEXIT_CRITICAL(&stateMux);
    return state;
}

OSPEED
void AudioSynthWavetable::update() {
    // Snapshot the control-plane state once. The DSP work then runs without
    // holding a critical section across a full audio block.
    const sample_data *sample;
    uint32_t localGeneration;
    uint32_t tonePhase;
    uint32_t toneIncrement;
    uint16_t toneAmplitude;
    envelopeStateEnum envelopeState;
    int32_t envelopeCount;
    int32_t envelopeMultiplier;
    int32_t envelopeIncrement;
    uint32_t vibratoCount;
    uint32_t vibratoPhase;
    int32_t vibratoPitchOffsetInitial;
    int32_t vibratoPitchOffsetSecond;
    uint32_t modulationCount;
    uint32_t modulationPhase;
    int32_t modulationPitchOffsetInitial;
    int32_t modulationPitchOffsetSecond;

    portENTER_CRITICAL(&stateMux);
    sample = current_sample;
    localGeneration = stateGeneration;
    tonePhase = tone_phase;
    toneIncrement = tone_incr;
    toneAmplitude = tone_amp;
    envelopeState = env_state;
    envelopeCount = env_count;
    envelopeMultiplier = env_mult;
    envelopeIncrement = env_incr;
    vibratoCount = vib_count;
    vibratoPhase = vib_phase;
    vibratoPitchOffsetInitial = vib_pitch_offset_init;
    vibratoPitchOffsetSecond = vib_pitch_offset_scnd;
    modulationCount = mod_count;
    modulationPhase = mod_phase;
    modulationPitchOffsetInitial = mod_pitch_offset_init;
    modulationPitchOffsetSecond = mod_pitch_offset_scnd;
    portEXIT_CRITICAL(&stateMux);

    if (envelopeState == STATE_IDLE || sample == nullptr) {
        return;
    }

    if (!sample->LOOP && tonePhase >= sample->MAX_PHASE) {
        portENTER_CRITICAL(&stateMux);
        if (stateGeneration == localGeneration) {
            env_state = STATE_IDLE;
            env_count = 0;
            env_mult = 0;
            env_incr = 0;
            ++stateGeneration;
        }
        portEXIT_CRITICAL(&stateMux);
        return;
    }

    audio_block_t *block = allocate();
    if (block == nullptr) {
        return;
    }

    bool sampleEnded = false;
    const int blockSize = AUDIO_BLOCK_SAMPLES;
    const int lfoPeriod = (LFO_PERIOD >= 1.0f) ? (int)LFO_PERIOD : 1;
    int sampleOffset = 0;

    while (sampleOffset < blockSize && !sampleEnded) {
        int32_t toneIncrementOffset = 0;

        if (vibratoCount++ > sample->VIBRATO_DELAY) {
            vibratoPhase += sample->VIBRATO_INCREMENT;
            const int32_t vibratoScale = (vibratoPhase & 0x80000000U)
                ? (int32_t)(0x40000000U + vibratoPhase)
                : (int32_t)(0x3FFFFFFFU - vibratoPhase);
            const int32_t vibratoPitchOffset = vibratoScale >= 0
                ? vibratoPitchOffsetInitial : vibratoPitchOffsetSecond;
            toneIncrementOffset = wavetable_macc32x32_rshift32_rounded(
                toneIncrementOffset, vibratoScale, vibratoPitchOffset);
        }

        int32_t modulationAmplitude = toneAmplitude;
        if (modulationCount++ > sample->MODULATION_DELAY) {
            modulationPhase += sample->MODULATION_INCREMENT;
            int32_t modulationScale = (modulationPhase & 0x80000000U)
                ? (int32_t)(0x40000000U + modulationPhase)
                : (int32_t)(0x3FFFFFFFU - modulationPhase);

            const int32_t modulationPitchOffset = modulationScale >= 0
                ? modulationPitchOffsetInitial : modulationPitchOffsetSecond;
            toneIncrementOffset = wavetable_macc32x32_rshift32_rounded(
                toneIncrementOffset, modulationScale, modulationPitchOffset);

            const int32_t modulationAmplitudeOffset = modulationScale >= 0
                ? sample->MODULATION_AMPLITUDE_INITIAL_GAIN
                : sample->MODULATION_AMPLITUDE_SECOND_GAIN;

            modulationScale = wavetable_mul32x32_rshift32(
                modulationScale, modulationAmplitudeOffset);
            modulationAmplitude = wavetable_macc32x16b(
                modulationAmplitude, modulationScale,
                (uint32_t)modulationAmplitude);
        }

        if (modulationAmplitude < 0) {
            modulationAmplitude = 0;
        } else if (modulationAmplitude > (int32_t)UINT16_MAX) {
            modulationAmplitude = UINT16_MAX;
        }

        const int segmentEnd = (sampleOffset + lfoPeriod < blockSize)
                                  ? sampleOffset + lfoPeriod : blockSize;

        for (; sampleOffset < segmentEnd; ++sampleOffset) {
            if (!sample->LOOP && tonePhase >= sample->MAX_PHASE) {
                sampleEnded = true;
                break;
            }

            const uint32_t sampleIndex = tonePhase >> (32 - sample->INDEX_BITS);
            const uint32_t phaseScale = (tonePhase << sample->INDEX_BITS) >> 16;
            const uint32_t scale = phaseScale & 0xFFFFU;

            // SoundFont samples are signed 16-bit PCM. Reading the pair
            // individually avoids unaligned 32-bit loads on Xtensa targets.
            const int32_t sample0 = sample->sample[sampleIndex];
            const int32_t sample1 = sample->sample[sampleIndex + 1U];
            const int32_t part0 = (sample0 * (int32_t)(0xFFFFU - scale)) >> 16;
            const int32_t part1 = (sample1 * (int32_t)scale) >> 16;
            const int32_t interpolatedSample = part0 + part1;
            const int32_t output = (int32_t)(
                ((int64_t)modulationAmplitude * interpolatedSample) >> 16);

            block->data[sampleOffset] = wavetable_saturate16(output);

            tonePhase += toneIncrement + (uint32_t)toneIncrementOffset;
            if (!sample->LOOP && tonePhase >= sample->MAX_PHASE) {
                sampleEnded = true;
                ++sampleOffset;
                break;
            }

            if (sample->LOOP && sample->LOOP_PHASE_LENGTH != 0U &&
                tonePhase >= sample->LOOP_PHASE_END) {
                tonePhase -= sample->LOOP_PHASE_LENGTH;
            }
        }
    }

    if (sampleEnded) {
        while (sampleOffset < blockSize) {
            block->data[sampleOffset++] = 0;
        }
    }

    // Apply the SoundFont volume envelope. Counts are measured in groups of
    // ENVELOPE_PERIOD samples; envelope levels advance once per output sample.
    int envelopeOffset = 0;
    while (envelopeOffset < blockSize) {
        bool zeroRemainder = false;

        while (envelopeCount <= 0) {
            switch (envelopeState) {
                case STATE_DELAY:
                    envelopeState = STATE_ATTACK;
                    envelopeCount = (int32_t)sample->ATTACK_COUNT;
                    if (envelopeCount <= 0) {
                        envelopeCount = 1;
                    }
                    envelopeIncrement = (int32_t)(
                        (int64_t)UNITY_GAIN /
                        ((int64_t)envelopeCount * ENVELOPE_PERIOD));
                    continue;

                case STATE_ATTACK:
                    envelopeMultiplier = UNITY_GAIN;
                    envelopeState = STATE_HOLD;
                    envelopeCount = (int32_t)sample->HOLD_COUNT;
                    envelopeIncrement = 0;
                    continue;

                case STATE_HOLD:
                    envelopeState = STATE_DECAY;
                    envelopeCount = (int32_t)sample->DECAY_COUNT;
                    if (envelopeCount <= 0) {
                        envelopeCount = 1;
                    }
                    envelopeIncrement = (int32_t)(
                        -((int64_t)sample->SUSTAIN_MULT) /
                        ((int64_t)envelopeCount * ENVELOPE_PERIOD));
                    continue;

                case STATE_DECAY:
                    envelopeMultiplier = UNITY_GAIN - sample->SUSTAIN_MULT;
                    envelopeState = envelopeMultiplier < UNITY_GAIN / UINT16_MAX
                                        ? STATE_RELEASE : STATE_SUSTAIN;
                    envelopeIncrement = 0;
                    continue;

                case STATE_SUSTAIN:
                    envelopeCount = INT32_MAX;
                    continue;

                case STATE_RELEASE:
                    envelopeState = STATE_IDLE;
                    envelopeCount = 0;
                    envelopeMultiplier = 0;
                    envelopeIncrement = 0;
                    zeroRemainder = true;
                    break;

                case STATE_IDLE:
                default:
                    zeroRemainder = true;
                    break;
            }
            break;
        }

        if (zeroRemainder || envelopeState == STATE_IDLE) {
            memset(block->data + envelopeOffset, 0,
                   (size_t)(blockSize - envelopeOffset) * sizeof(block->data[0]));
            break;
        }

        int samplesThisGroup = ENVELOPE_PERIOD;
        if (samplesThisGroup > blockSize - envelopeOffset) {
            samplesThisGroup = blockSize - envelopeOffset;
        }

        for (int i = 0; i < samplesThisGroup; ++i) {
            envelopeMultiplier = wavetable_clamp_envelope(
                (int64_t)envelopeMultiplier + envelopeIncrement);

            const int32_t scaled = (int32_t)(
                ((int64_t)block->data[envelopeOffset] * envelopeMultiplier) >> 31);
            block->data[envelopeOffset] = wavetable_saturate16(scaled);
            ++envelopeOffset;
        }

        --envelopeCount;
    }

    if (sampleEnded) {
        envelopeState = STATE_IDLE;
        envelopeCount = 0;
        envelopeMultiplier = 0;
        envelopeIncrement = 0;
    }

    // Commit the rendered state only if a new note/stop/frequency command did
    // not arrive while this block was being generated.
    portENTER_CRITICAL(&stateMux);
    if (stateGeneration == localGeneration) {
        tone_phase = tonePhase;
        env_state = envelopeState;
        env_count = envelopeCount;
        env_mult = envelopeMultiplier;
        env_incr = envelopeIncrement;

        if (env_state != STATE_IDLE) {
            vib_count = vibratoCount;
            vib_phase = vibratoPhase;
            mod_count = modulationCount;
            mod_phase = modulationPhase;
        } else {
            vib_count = mod_count = 0;
            vib_phase = mod_phase = (uint32_t)TRIANGLE_INITIAL_PHASE;
        }
    }
    portEXIT_CRITICAL(&stateMux);

    transmit(block);
    release(block);
}
