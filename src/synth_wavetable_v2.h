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

    // Fixed 18/14 phase format: 18-bit sample index, 14-bit fraction.
    static const uint8_t SAMPLE_INDEX_BITS = 18;
    static const uint8_t SAMPLE_FRACTION_BITS = 14;
    static const uint32_t MAX_SAMPLE_COUNT = 1UL << SAMPLE_INDEX_BITS;
    static const uint32_t MAX_SAMPLE_FRAMES = MAX_SAMPLE_COUNT - 1UL;

    struct sample_data {
        const int16_t *SAMPLE; // Mono PCM or the left channel of a stereo pair.
        // Optional linked right-channel PCM. nullptr keeps this layer mono.
        // Both arrays must have SAMPLE_COUNT frames and matching sample positions.
        const int16_t *SAMPLE_RIGHT;
        // Number of stored PCM frames per channel, including the interpolation guard frame.
        const uint32_t SAMPLE_COUNT;
        const float PER_HERTZ_PHASE_INCREMENT;
        // Exclusive end position in 32-bit phase units. Zero derives it from
        // SAMPLE_COUNT using the fixed 14-bit fractional phase.
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

        sample_data(const int16_t *sample, uint32_t sampleCount,
                    float perHertzPhaseIncrement, uint32_t maxPhase = 0,
                    LoopMode loopMode = LoopMode::ONE_SHOT,
                    uint32_t loopPhaseEnd = 0, uint32_t loopPhaseLength = 0,
                    uint16_t initialAttenuationScalar = UINT16_MAX,
                    uint32_t delayCount = 0, uint32_t attackCount = 1,
                    uint32_t holdCount = 0, uint32_t decayCount = 1,
                    uint32_t releaseCount = 1, int32_t sustainMult = 0,
                    const int16_t *sampleRight = nullptr)
        : SAMPLE(sample), SAMPLE_RIGHT(sampleRight), SAMPLE_COUNT(sampleCount),
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
        // SF2 exclusive class. A value of zero disables exclusive voice killing.
        const uint16_t EXCLUSIVE_CLASS;

        zone_data(uint8_t keyLow, uint8_t keyHigh, uint8_t velocityLow,
                  uint8_t velocityHigh, const sample_data &sample,
                  uint16_t exclusiveClass = 0)
        : KEY_LOW(keyLow), KEY_HIGH(keyHigh), VELOCITY_LOW(velocityLow),
          VELOCITY_HIGH(velocityHigh), SAMPLE(&sample),
          EXCLUSIVE_CLASS(exclusiveClass) {}
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

    // Audio output 0 is left and output 1 is right. Mono voices are mirrored
    // to both output indices using the same audio block.
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
        const sample_data *sample;
        uint16_t exclusiveClass;
        uint32_t phase;
        uint32_t phaseIncrement;
        uint16_t amplitude;

        EnvelopeState envelopeState;
        int32_t envelopeCount;
        int32_t envelopeMultiplier;
        int32_t envelopeIncrement;
        uint8_t envelopeSubCount;

        // Exact amplitude-scaled release ramp: 16-bit integer gain plus
        // a 31-bit fractional accumulator. The step uses the same split.
        uint16_t releaseGain;
        uint16_t releaseGainStep;
        uint32_t releaseGainFraction;
        uint32_t releaseGainStepFraction;
        uint32_t releaseSamplesRemaining;

        uint32_t delayCount;
        uint32_t attackCount;
        uint32_t holdCount;
        uint32_t decayCount;
        uint32_t releaseCount;
    };

    uint32_t scaleSampleCount(uint32_t value) const;
    uint32_t maximumPhase(const sample_data &sample) const;
    void beginReleaseLocked(SampleVoice &voice);
    void beginReleaseLocked(SampleVoice &voice, uint32_t releaseCount);
    void releaseExclusiveVoicesLocked(uint8_t channel,
                                      const zone_data *const *zones,
                                      uint8_t zoneCount);
    void compactInactiveVoices();
    void releaseNoteLocked(uint8_t noteSlot);
    void stealNoteLocked(uint8_t noteSlot);
    void renderVoice(SampleVoice &voice, int32_t *mix) const;
    void renderVoiceStereo(SampleVoice &voice, int32_t *mixLeft,
                           int32_t *mixRight) const;
    void updateStereo(uint16_t masterGain);

    // Compile-time loop policies keep loop/end checks out of sample loops.
    // Stereo is a separate specialization, so the mono hot loop has no
    // per-sample channel test or right-channel work.
    template <bool StopAtEnd, bool WrapLoop, bool Stereo>
    void renderVoicePath(SampleVoice &voice, int32_t *mixLeft,
                         int32_t *mixRight) const;

    // Control-plane lock only. AudioTask accesses this state directly and is
    // pinned to the same core at higher priority than all public control calls.
    // Do not call control methods from another core or an ISR without revisiting
    // this synchronization model.
    mutable portMUX_TYPE stateMux_ = portMUX_INITIALIZER_UNLOCKED;

    const instrument_data *instrument_ = nullptr;
    NoteState notes_[MAX_NOTES] = {};
    SampleVoice sampleVoices_[MAX_SAMPLE_VOICES] = {};
    // Dense active-slot list keeps update() work proportional to actual polyphony.
    uint16_t activeVoiceSlots_[MAX_SAMPLE_VOICES] = {};
    uint16_t activeVoiceCount_ = 0;
    uint16_t activeStereoVoiceCount_ = 0;
    bool sustain_[16] = {};

    float sampleCountScale_ = 1.0f;
    float phaseIncrementScale_ = 1.0f;
    uint16_t masterGain_ = UINT16_MAX;
    // 300 ms exclusive-class release in ENVELOPE_PERIOD units, precomputed
    // when the instrument/sample rate is configured.
    uint32_t exclusiveReleaseCount_ = 0;
    uint32_t nextToken_ = 0;
    uint32_t nextAge_ = 0;
};
