/*
 * ESP-beatsy Audio Library
 *
 * Three compact, sample-free percussion synthesizers:
 * - AudioSynthKick: pitch-swept sine body with a short noise beater transient.
 * - AudioSynthTom: two inharmonic membrane modes with a pitch envelope.
 * - AudioSynthSnare: filtered noise burst plus two tuned shell modes.
 *
 * All controls are normalized or expressed in Hz/milliseconds as documented.
 */

#pragma once
#ifndef SYNTH_DRUMKIT_H
#define SYNTH_DRUMKIT_H

#include "AudioStream.h"

#include <stdint.h>

// Analog-style bass drum. noteOn() restarts the current voice.
// length() is the approximate -60 dB amplitude decay time.
class AudioSynthKick : public AudioStream {
  public:
    AudioSynthKick();

    void noteOn(uint8_t velocity = 100);
    void frequency(float frequencyHz);
    void length(float milliseconds);
    void pitchSweep(float octaves);
    void pitchDecay(float milliseconds);
    void click(float level);

    void update() override;

  private:
    uint32_t phaseIncrement = 0;
    float amplitudeDecay = 0.0f;
    float pitchRatio = 1.0f;
    float pitchDecayCoefficient = 0.0f;
    float clickLevel = 0.16f;
    float clickDecayCoefficient = 0.0f;

    uint32_t phase = 0;
    float amplitudeEnvelope = 0.0f;
    float pitchEnvelope = 0.0f;
    float clickEnvelope = 0.0f;
    float velocityGain = 0.0f;
    uint32_t noiseState = 0xA341316CU;
    uint32_t triggerGeneration = 0;

    portMUX_TYPE configMux = portMUX_INITIALIZER_UNLOCKED;
    portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
};

// Three-mode tom based on the first circular-membrane modes (1.000, 1.593,
// 2.136). Higher modes decay faster; a short filtered-noise transient models
// the stick attack. secondMix controls both upper modes.
class AudioSynthTom : public AudioStream {
  public:
    AudioSynthTom();

    void noteOn(uint8_t velocity = 100);
    void frequency(float frequencyHz);
    void length(float milliseconds);
    void pitchSweep(float octaves);
    void pitchDecay(float milliseconds);
    void secondMix(float level);
    void attack(float level);

    void update() override;

  private:
    uint32_t phaseIncrement = 0;
    uint32_t secondPhaseIncrement = 0;
    uint32_t thirdPhaseIncrement = 0;
    float amplitudeDecay = 0.0f;
    float secondAmplitudeDecay = 0.0f;
    float thirdAmplitudeDecay = 0.0f;
    float pitchRatio = 1.0f;
    float secondPitchRatio = 1.0f;
    float thirdPitchRatio = 1.0f;
    float pitchDecayCoefficient = 0.0f;
    float secondMixLevel = 0.55f;
    float attackLevel = 0.22f;
    float noiseFastCoefficient = 0.0f;
    float noiseSlowCoefficient = 0.0f;
    float attackDecayCoefficient = 0.0f;

    uint32_t phase = 0;
    uint32_t secondPhase = 0;
    uint32_t thirdPhase = 0;
    float amplitudeEnvelope = 0.0f;
    float secondAmplitudeEnvelope = 0.0f;
    float thirdAmplitudeEnvelope = 0.0f;
    float pitchEnvelope = 0.0f;
    float attackEnvelope = 0.0f;
    float noiseFast = 0.0f;
    float noiseSlow = 0.0f;
    float velocityGain = 0.0f;
    uint32_t noiseState = 0x9E3779B9U;
    uint32_t triggerGeneration = 0;

    portMUX_TYPE configMux = portMUX_INITIALIZER_UNLOCKED;
    portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
};

// Snare with a band-pass-like noise burst and two damped shell modes.
// snappy controls the wire/noise component; tone controls the shell.
class AudioSynthSnare : public AudioStream {
  public:
    AudioSynthSnare();

    void noteOn(uint8_t velocity = 100);
    void frequency(float frequencyHz);
    void length(float milliseconds);
    void snappy(float level);
    void tone(float level);

    void update() override;

  private:
    uint32_t phaseIncrement = 0;
    uint32_t secondPhaseIncrement = 0;
    float bodyDecay = 0.0f;
    float noiseDecay = 0.0f;
    float noiseFastCoefficient = 0.0f;
    float noiseSlowCoefficient = 0.0f;
    float snappyLevel = 0.85f;
    float toneLevel = 0.62f;

    uint32_t phase = 0;
    uint32_t secondPhase = 0;
    float bodyEnvelope = 0.0f;
    float noiseEnvelope = 0.0f;
    float noiseFast = 0.0f;
    float noiseSlow = 0.0f;
    float velocityGain = 0.0f;
    uint32_t noiseState = 0xC8013EA4U;
    uint32_t triggerGeneration = 0;

    portMUX_TYPE configMux = portMUX_INITIALIZER_UNLOCKED;
    portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
};

#endif // SYNTH_DRUMKIT_H
