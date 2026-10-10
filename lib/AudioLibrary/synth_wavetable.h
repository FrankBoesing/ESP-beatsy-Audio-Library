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

#pragma once

#include "AudioStream.h"

#include <math.h>
#include <stdint.h>

#define WAVETABLE_CENTS_SHIFT(C) (pow(2.0, (C) / 1200.0))
#define WAVETABLE_NOTE_TO_FREQUENCY(N) (440.0 * pow(2.0, ((N) - 69) / 12.0))
#define WAVETABLE_DECIBEL_SHIFT(dB) (pow(10.0, (dB) / 20.0))

class AudioSynthWavetable : public AudioStream {
  public:
    struct sample_data {
        // Sample values
        const int16_t *sample;
        const bool LOOP;
        const int INDEX_BITS;
        const float PER_HERTZ_PHASE_INCREMENT;
        const uint32_t MAX_PHASE;
        const uint32_t LOOP_PHASE_END;
        const uint32_t LOOP_PHASE_LENGTH;
        const uint16_t INITIAL_ATTENUATION_SCALAR;

        // Volume envelope values
        const uint32_t DELAY_COUNT;
        const uint32_t ATTACK_COUNT;
        const uint32_t HOLD_COUNT;
        const uint32_t DECAY_COUNT;
        const uint32_t RELEASE_COUNT;
        const int32_t SUSTAIN_MULT;

        // Vibrato values
        const uint32_t VIBRATO_DELAY;
        const uint32_t VIBRATO_INCREMENT;
        const float VIBRATO_PITCH_COEFFICIENT_INITIAL;
        const float VIBRATO_PITCH_COEFFICIENT_SECOND;

        // Modulation values
        const uint32_t MODULATION_DELAY;
        const uint32_t MODULATION_INCREMENT;
        const float MODULATION_PITCH_COEFFICIENT_INITIAL;
        const float MODULATION_PITCH_COEFFICIENT_SECOND;
        const int32_t MODULATION_AMPLITUDE_INITIAL_GAIN;
        const int32_t MODULATION_AMPLITUDE_SECOND_GAIN;
    };

    static const int32_t UNITY_GAIN = INT32_MAX;
    static constexpr float SAMPLES_PER_MSEC = AUDIO_SAMPLE_RATE_EXACT / 1000.0f;
    static const int32_t LFO_SMOOTHNESS = 3;
    static constexpr float LFO_PERIOD = AUDIO_BLOCK_SAMPLES / (1 << (LFO_SMOOTHNESS - 1));
    static const int32_t ENVELOPE_PERIOD = 8;

    struct instrument_data {
        const uint8_t sample_count;
        const uint8_t *sample_note_ranges;
        const sample_data *samples;
    };

    enum { DEFAULT_AMPLITUDE = 90 };
    enum { TRIANGLE_INITIAL_PHASE = -0x40000000 };
    enum envelopeStateEnum {
        STATE_IDLE,
        STATE_DELAY,
        STATE_ATTACK,
        STATE_HOLD,
        STATE_DECAY,
        STATE_SUSTAIN,
        STATE_RELEASE
    };

    AudioSynthWavetable() : AudioStream(0, nullptr) {}

    // The instrument and its sample/note-range arrays must remain alive for
    // the entire time this object can play them (typically static SoundFont data).
    void setInstrument(const instrument_data &newInstrument);

    void amplitude(float value);

    static float midi_volume_transform(int midi_amp);
    static float noteToFreq(int note);
    static int freqToNote(float freq);

    void stop();
    void playFrequency(float freq, int amp = DEFAULT_AMPLITUDE);
    void playNote(int note, int amp = DEFAULT_AMPLITUDE);
    bool isPlaying();
    void setFrequency(float freq);
    void update() override;

    envelopeStateEnum getEnvState();

  private:
    void setState(int note, int amp, float freq);
    void setFrequencyLocked(float freq);

    audio_block_t *inputQueueArray[1] = {};

    // Protect control-plane changes. update() renders from a local snapshot and
    // writes the evolving state back only if no newer command arrived meanwhile.
    portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
    uint32_t stateGeneration = 0;

    const instrument_data *instrument = nullptr;
    const sample_data *current_sample = nullptr;

    uint32_t tone_phase = 0;
    uint32_t tone_incr = 0;
    uint16_t tone_amp = 0;

    envelopeStateEnum env_state = STATE_IDLE;
    int32_t env_count = 0;
    int32_t env_mult = 0;
    int32_t env_incr = 0;

    uint32_t vib_count = 0;
    uint32_t vib_phase = (uint32_t)TRIANGLE_INITIAL_PHASE;
    int32_t vib_pitch_offset_init = 0;
    int32_t vib_pitch_offset_scnd = 0;

    uint32_t mod_count = 0;
    uint32_t mod_phase = (uint32_t)TRIANGLE_INITIAL_PHASE;
    int32_t mod_pitch_offset_init = 0;
    int32_t mod_pitch_offset_scnd = 0;
};
