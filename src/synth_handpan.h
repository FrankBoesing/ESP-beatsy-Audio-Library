#pragma once
#ifndef synth_handpan_h_
#define synth_handpan_h_

#include "AudioStream.h"

#include <stdint.h>

// Hybrid handpan synthesizer: a built-in, procedurally generated PCM16 attack
// plus damped resonators for the sustained body and metallic ring. A caller may
// override the built-in attack with its own PCM16 data. Each strike uses a short
// onset ramp to avoid an abrupt resonator start. Configure note slots before
// starting the audio graph; caller-supplied sample memory must remain valid.
class AudioSynthHandpan : public AudioStream {
  public:
    static constexpr uint8_t MAX_NOTES = 16;
    static constexpr uint8_t MAX_MODES = 6;
    static constexpr uint8_t MAX_ATTACK_VOICES = 8;
    static constexpr uint8_t MAX_STRIKE_VOICES = 32;
    static constexpr uint8_t MAX_PENDING_EVENTS = 16;
    static constexpr uint32_t MAX_ATTACK_SAMPLES = 8192;

    struct ModeConfig {
        float frequencyHz;    // Resonance frequency in Hz.
        float gain;           // Peak amplitude in normalized output units (0..1).
        float decaySeconds;   // T60: time to decay by 60 dB in amplitude.
        float phaseRadians;   // Initial phase, used to shape the strike response.
    };

    AudioSynthHandpan();

    // Configure a logical pad (slot 0..15). Pass up to six modal components,
    // usually three body modes followed by three metallic ring modes. Each mode
    // has an independent frequency, gain, phase and decay time.
    // This overload uses the built-in soft 30 ms finger-contact transient,
    // generated once in library-owned RAM. No sample data is needed in the sketch.
    // Call before AudioMemory()/updates, after selecting the sample rate.
    bool configureNote(uint8_t slot, const ModeConfig *modes, uint8_t modeCount);

    // Optional custom attack sample: signed mono PCM16, retained but not copied.
    // Pass nullptr and a count of zero for a resonator-only sound. Caller-owned
    // memory must remain valid during playback. Set attackGain to 0.0f to disable
    // a supplied sample. Call before AudioMemory()/updates, after selecting the rate.
    bool configureNote(uint8_t slot, const ModeConfig *modes, uint8_t modeCount,
                       const int16_t *attackSample,
                       uint32_t attackSampleCount,
                       uint32_t attackSampleRate = 44100,
                       float attackGain = 0.09f);

    // Queue a strike. velocity is MIDI-like, 0..127. Returns false if the slot
    // is not configured or the fixed event queue is full. Each strike is faded
    // in over 5 ms as a separate resonator voice, then merged into the pad's
    // long-lived ring; existing tails are not reset or ducked.
    bool noteOn(uint8_t slot, uint8_t velocity = 100);

    // Enable the soft saturation used by the H2 listening prototype. Amount is
    // clamped to 0..1; zero bypasses the non-linear stage (default), one enables it.
    // Set before playback to avoid concurrent changes to the audio update loop.
    void setSaturation(float amount);

    void update() override;

  private:
    struct Resonator {
        ModeConfig config{};
        float coefficient1 = 0.0f;
        float coefficient2 = 0.0f;
        float sinPhase = 0.0f;
        float previousStateScale = 0.0f;
        float y1 = 0.0f;
        float y2 = 0.0f;
        bool active = false;
    };

    struct Note {
        Resonator modes[MAX_MODES]{};
        uint8_t modeCount = 0;
        bool configured = false;
        const int16_t *attackSample = nullptr;
        uint32_t attackSampleCount = 0;
        uint32_t attackIncrementQ16 = 0;
        uint16_t onsetRampSamples = 0;
        float attackGain = 0.09f;
    };

    struct AttackVoice {
        const int16_t *sample = nullptr;
        uint32_t sampleCount = 0;
        uint32_t positionQ16 = 0;
        uint32_t incrementQ16 = 0;
        float gain = 0.0f;
        bool active = false;
    };

    // A strike is initially rendered as an independent oscillator voice. Its
    // output is smoothly faded in, then its state is merged into the pad's
    // long-lived resonators after the ramp has finished.
    struct StrikeVoice {
        uint8_t slot = 0;
        uint16_t position = 0;
        bool active = false;
        float y1[MAX_MODES] = {};
        float y2[MAX_MODES] = {};
    };

    struct NoteEvent {
        uint8_t slot = 0;
        uint8_t velocity = 0;
    };

    struct ActiveModeRef {
        uint8_t slot = 0;
        uint8_t mode = 0;
    };

    static constexpr float OUTPUT_SCALE = 32768.0f;
    static constexpr float SILENCE_THRESHOLD = 1.0e-7f;
    static constexpr uint32_t ONSET_RAMP_US = 5000;

    void triggerNote(uint8_t slot, uint8_t velocity);
    void startAttack(uint8_t slot, float velocity);
    bool hasActiveSound() const;
    bool popEvent(NoteEvent &event);

    audio_block_t *inputQueueArray[1] = {};
    Note notes[MAX_NOTES]{};
    AttackVoice attackVoices[MAX_ATTACK_VOICES]{};
    StrikeVoice strikeVoices[MAX_STRIKE_VOICES]{};
    uint8_t activeStrikeVoiceIndices[MAX_STRIKE_VOICES]{};
    uint8_t activeStrikeVoiceCount = 0;
    ActiveModeRef activeModes[MAX_NOTES * MAX_MODES]{};
    uint8_t activeModeCount = 0;
    float saturationAmount = 0.0f;

    // Block accumulator lets each resonator keep its state and coefficients in
    // local variables for a whole block instead of reloading them per sample.
    float mixBuffer[AUDIO_BLOCK_SAMPLES] = {};

    NoteEvent eventQueue[MAX_PENDING_EVENTS]{};
    uint8_t eventRead = 0;
    uint8_t eventWrite = 0;
    portMUX_TYPE eventMux = portMUX_INITIALIZER_UNLOCKED;
};

#endif
