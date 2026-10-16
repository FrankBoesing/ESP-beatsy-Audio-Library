#pragma once
#ifndef synth_handpan_h_
#define synth_handpan_h_

#include "AudioStream.h"

#include <stdint.h>

// Hybrid handpan synthesizer: a short, caller-supplied PCM16 attack sample plus
// a bank of damped resonators for the sustained body and metallic ring.
// Configure note slots before starting the audio graph. Sample memory must
// remain valid for as long as this object may play it (const flash data is OK).
class AudioSynthHandpan : public AudioStream {
  public:
    static constexpr uint8_t MAX_NOTES = 16;
    static constexpr uint8_t MAX_MODES = 6;
    static constexpr uint8_t MAX_ATTACK_VOICES = 8;
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
    // usually three body modes followed by three short-lived metallic modes.
    // attackSample is signed mono PCM16, not copied by this object. Use nullptr
    // and a count of zero to synthesize the resonances without a sample attack.
    // Call before AudioMemory()/starting updates, after selecting the sample rate.
    bool configureNote(uint8_t slot, const ModeConfig *modes, uint8_t modeCount,
                       const int16_t *attackSample = nullptr,
                       uint32_t attackSampleCount = 0,
                       uint32_t attackSampleRate = 44100,
                       float attackGain = 0.09f);

    // Queue a strike. velocity is MIDI-like, 0..127. Returns false if the slot
    // is not configured or the fixed event queue is full. Resonators are not
    // reset by repeated strikes: each event adds energy to the existing ring.
    bool noteOn(uint8_t slot, uint8_t velocity = 100);

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

    void triggerNote(uint8_t slot, uint8_t velocity);
    void startAttack(uint8_t slot, float velocity);
    bool hasActiveSound() const;
    bool popEvent(NoteEvent &event);

    audio_block_t *inputQueueArray[1] = {};
    Note notes[MAX_NOTES]{};
    AttackVoice attackVoices[MAX_ATTACK_VOICES]{};
    ActiveModeRef activeModes[MAX_NOTES * MAX_MODES]{};
    uint8_t activeModeCount = 0;

    // Block accumulator lets each resonator keep its state and coefficients in
    // local variables for a whole block instead of reloading them per sample.
    float mixBuffer[AUDIO_BLOCK_SAMPLES] = {};

    NoteEvent eventQueue[MAX_PENDING_EVENTS]{};
    uint8_t eventRead = 0;
    uint8_t eventWrite = 0;
    portMUX_TYPE eventMux = portMUX_INITIALIZER_UNLOCKED;
};

#endif
