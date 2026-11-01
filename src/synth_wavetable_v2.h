/*
 * ESP-beatsy Audio Library
 *
 * Polyphonic, zone-based sample wavetable synthesizer.
 * This is an independent successor to AudioSynthWavetable; the legacy class
 * and its instrument_data format remain unchanged.
 */

#pragma once

#include "AudioStream.h"

#include <stdint.h>

class AudioSynthWavetableV2 : public AudioStream {
  public:
    enum class LoopMode : uint8_t {
        ONE_SHOT = 0,
        CONTINUOUS = 1,
        UNTIL_RELEASE = 2
    };

    enum EnvelopeState : uint8_t {
        STATE_IDLE,
        STATE_DELAY,
        STATE_ATTACK,
        STATE_HOLD,
        STATE_DECAY,
        STATE_SUSTAIN,
        STATE_RELEASE
    };

    // Fixed limits keep update() allocation-free and make worst-case memory
    // and CPU usage predictable. 32 notes x 4 layers = 128 sample instances.
    static const uint8_t MAX_NOTES = 32;
    static const uint8_t MAX_SAMPLE_LAYERS_PER_NOTE = 4;
    static const uint16_t MAX_SAMPLE_VOICES = MAX_NOTES * MAX_SAMPLE_LAYERS_PER_NOTE;

    static const int32_t UNITY_GAIN = INT32_MAX;
    static const uint8_t ENVELOPE_PERIOD = 8;

    struct sample_data {
        const int16_t *SAMPLE;
        // Number of stored PCM frames, including the interpolation guard frame.
        const uint32_t SAMPLE_COUNT;
        const uint8_t INDEX_BITS;
        const float PER_HERTZ_PHASE_INCREMENT;
        // Exclusive end position in 32-bit phase units. Zero derives it from
        // SAMPLE_COUNT and INDEX_BITS.
        const uint32_t MAX_PHASE;
        const LoopMode LOOP_MODE;
        const uint32_t LOOP_PHASE_END;
        const uint32_t LOOP_PHASE_LENGTH;
        const uint16_t INITIAL_ATTENUATION_SCALAR;

        // Envelope times are expressed in groups of ENVELOPE_PERIOD samples.
        const uint32_t DELAY_COUNT;
        const uint32_t ATTACK_COUNT;
        const uint32_t HOLD_COUNT;
        const uint32_t DECAY_COUNT;
        const uint32_t RELEASE_COUNT;
        // Q31 attenuation from unity: 0 = full sustain, UNITY_GAIN = silence.
        const int32_t SUSTAIN_MULT;

        sample_data(const int16_t *sample, uint32_t sampleCount, uint8_t indexBits,
                    float perHertzPhaseIncrement, uint32_t maxPhase = 0,
                    LoopMode loopMode = LoopMode::ONE_SHOT,
                    uint32_t loopPhaseEnd = 0, uint32_t loopPhaseLength = 0,
                    uint16_t initialAttenuationScalar = UINT16_MAX,
                    uint32_t delayCount = 0, uint32_t attackCount = 1,
                    uint32_t holdCount = 0, uint32_t decayCount = 1,
                    uint32_t releaseCount = 1, int32_t sustainMult = 0)
        : SAMPLE(sample), SAMPLE_COUNT(sampleCount), INDEX_BITS(indexBits),
          PER_HERTZ_PHASE_INCREMENT(perHertzPhaseIncrement), MAX_PHASE(maxPhase),
          LOOP_MODE(loopMode), LOOP_PHASE_END(loopPhaseEnd),
          LOOP_PHASE_LENGTH(loopPhaseLength),
          INITIAL_ATTENUATION_SCALAR(initialAttenuationScalar),
          DELAY_COUNT(delayCount), ATTACK_COUNT(attackCount),
          HOLD_COUNT(holdCount), DECAY_COUNT(decayCount),
          RELEASE_COUNT(releaseCount), SUSTAIN_MULT(sustainMult) {}
    };

    struct zone_data {
        const uint8_t KEY_LOW;
        const uint8_t KEY_HIGH;
        const uint8_t VELOCITY_LOW;
        const uint8_t VELOCITY_HIGH;
        const sample_data *SAMPLE;

        zone_data(uint8_t keyLow, uint8_t keyHigh, uint8_t velocityLow,
                  uint8_t velocityHigh, const sample_data &sample)
        : KEY_LOW(keyLow), KEY_HIGH(keyHigh), VELOCITY_LOW(velocityLow),
          VELOCITY_HIGH(velocityHigh), SAMPLE(&sample) {}
    };

    struct instrument_data {
        const zone_data *ZONES;
        const uint16_t ZONE_COUNT;

        instrument_data(const zone_data *zones, uint16_t zoneCount)
        : ZONES(zones), ZONE_COUNT(zoneCount) {}
    };

    AudioSynthWavetableV2() : AudioStream(0, nullptr) {}

    // Instrument/zone/sample descriptors and their PCM arrays must remain
    // valid for the lifetime of playback. Call after AudioMemory() and again
    // after a global sample-rate change. setInstrument() stops all voices.
    bool setInstrument(const instrument_data &newInstrument);

    // Starts all matching key/velocity zones, up to MAX_SAMPLE_LAYERS_PER_NOTE.
    // Returns the number of sample layers started; zero means no matching zone
    // or no available note slot.
    uint8_t noteOn(int note, int velocity = 127, uint8_t channel = 0);
    bool noteOff(int note, uint8_t channel = 0);

    // channel == -1 means all MIDI channels.
    void allNotesOff(int channel = -1);
    void allSoundOff(int channel = -1);
    void sustainPedal(uint8_t channel, bool enabled);

    // Master gain affects already-playing and newly-started notes.
    void amplitude(float value);

    // stop() is the release-based equivalent of allNotesOff().
    void stop() { allNotesOff(-1); }

    bool isPlaying() const;
    uint8_t activeNoteCount() const;
    uint16_t activeSampleCount() const;

    static float noteToFreq(int note);

    void update() override;

  private:
    struct NoteState {
        bool active;
        bool keyDown;
        bool sustainHeld;
        uint8_t note;
        uint8_t channel;
        uint8_t layerCount;
        uint32_t token;
        uint32_t age;
    };

    struct SampleVoice {
        bool active;
        bool released;
        uint8_t noteSlot;
        uint32_t noteToken;
        uint32_t generation;
        const sample_data *sample;
        uint32_t phase;
        uint32_t phaseIncrement;
        uint16_t amplitude;

        EnvelopeState envelopeState;
        int32_t envelopeCount;
        int32_t envelopeMultiplier;
        int32_t envelopeIncrement;
        uint8_t envelopeSubCount;

        uint32_t delayCount;
        uint32_t attackCount;
        uint32_t holdCount;
        uint32_t decayCount;
        uint32_t releaseCount;
    };

    uint32_t scaleSampleCount(uint32_t value) const;
    uint32_t maximumPhase(const sample_data &sample) const;
    void beginReleaseLocked(SampleVoice &voice);
    void releaseNoteLocked(uint8_t noteSlot);
    void stealNoteLocked(uint8_t noteSlot);
    bool advanceEnvelope(SampleVoice &voice) const;
    void renderVoice(SampleVoice &voice, int32_t *mix) const;

    mutable portMUX_TYPE stateMux_ = portMUX_INITIALIZER_UNLOCKED;

    const instrument_data *instrument_ = nullptr;
    NoteState notes_[MAX_NOTES] = {};
    SampleVoice sampleVoices_[MAX_SAMPLE_VOICES] = {};
    bool sustain_[16] = {};

    float sampleCountScale_ = 1.0f;
    float phaseIncrementScale_ = 1.0f;
    uint16_t masterGain_ = UINT16_MAX;
    uint32_t nextToken_ = 0;
    uint32_t nextAge_ = 0;
};
