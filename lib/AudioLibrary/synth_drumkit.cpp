#include "synth_drumkit.h"

#include <math.h>
#include <stdint.h>

#include "utility/dspinst.h"

namespace {

constexpr float DRUM_TWO_PI = 6.2831853071795864769f;
constexpr float SILENCE_THRESHOLD = 1.0e-5f;
constexpr float MAX_DECAY_MS = 5000.0f;

// A phase accumulator uses one full uint32_t cycle. The sine helper returns
// Q15.16, so the high word is the signed Q15 sample.
uint32_t phaseIncrementForFrequency(float frequencyHz, float fs)
{
    return (uint32_t)(frequencyHz * (4294967296.0f / fs));
}

// length is specified as the time to decay by approximately 60 dB in amplitude.
float amplitudeDecayCoefficient(float milliseconds, float fs)
{
    const float samples = milliseconds * (fs * 0.001f);
    return expf(-6.907755279f / samples);
}

// pitchDecay is an exponential time constant, not a -60 dB duration.
float timeConstantCoefficient(float milliseconds, float fs)
{
    const float samples = milliseconds * (fs * 0.001f);
    return expf(-1.0f / samples);
}

float clampLevel(float value)
{
    if (value < 0.0f) return 0.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

float noiseUnit(uint32_t &state)
{
    state = state * 1664525U + 1013904223U;
    return ((float)(state >> 8) * (1.0f / 8388607.5f)) - 1.0f;
}

float sineUnit(uint32_t phase)
{
    return (float)(sin_q15_phase_q16(phase) >> 16) * (1.0f / 32768.0f);
}

int16_t toPcm16(float sample)
{
    if (sample > 0.999969f) sample = 0.999969f;
    else if (sample < -1.0f) sample = -1.0f;
    return (int16_t)(sample * 32767.0f);
}

} // namespace

// -----------------------------------------------------------------------------
// Kick
// -----------------------------------------------------------------------------

AudioSynthKick::AudioSynthKick() : AudioStream(0, nullptr)
{
    frequency(52.0f);
    length(210.0f);
    pitchSweep(2.0f);
    pitchDecay(27.0f);
    click(0.16f);
}

void AudioSynthKick::frequency(float frequencyHz)
{
    if (!isfinite(frequencyHz)) return;

    const float fs = sampleRate();
    if (frequencyHz < 20.0f) frequencyHz = 20.0f;
    // Keep the full pitch sweep below Nyquist even at the maximum setting.
    if (frequencyHz > fs * 0.05f) frequencyHz = fs * 0.05f;

    const uint32_t increment = phaseIncrementForFrequency(frequencyHz, fs);
    const float clickDecay = timeConstantCoefficient(0.65f, fs);
    portENTER_CRITICAL(&configMux);
    phaseIncrement = increment;
    clickDecayCoefficient = clickDecay;
    portEXIT_CRITICAL(&configMux);
}

void AudioSynthKick::length(float milliseconds)
{
    if (!isfinite(milliseconds)) return;
    if (milliseconds < 5.0f) milliseconds = 5.0f;
    if (milliseconds > MAX_DECAY_MS) milliseconds = MAX_DECAY_MS;

    const float coefficient = amplitudeDecayCoefficient(milliseconds, sampleRate());
    portENTER_CRITICAL(&configMux);
    amplitudeDecay = coefficient;
    portEXIT_CRITICAL(&configMux);
}

void AudioSynthKick::pitchSweep(float octaves)
{
    if (!isfinite(octaves)) return;
    if (octaves < 0.0f) octaves = 0.0f;
    if (octaves > 3.0f) octaves = 3.0f;

    const float ratio = powf(2.0f, octaves);
    portENTER_CRITICAL(&configMux);
    pitchRatio = ratio;
    portEXIT_CRITICAL(&configMux);
}

void AudioSynthKick::pitchDecay(float milliseconds)
{
    if (!isfinite(milliseconds)) return;
    if (milliseconds < 1.0f) milliseconds = 1.0f;
    if (milliseconds > 250.0f) milliseconds = 250.0f;

    const float coefficient = timeConstantCoefficient(milliseconds, sampleRate());
    portENTER_CRITICAL(&configMux);
    pitchDecayCoefficient = coefficient;
    portEXIT_CRITICAL(&configMux);
}

void AudioSynthKick::click(float level)
{
    if (!isfinite(level)) return;

    level = clampLevel(level);
    portENTER_CRITICAL(&configMux);
    clickLevel = level;
    portEXIT_CRITICAL(&configMux);
}

void AudioSynthKick::noteOn(uint8_t velocity)
{
    if (velocity == 0) return;

    portENTER_CRITICAL(&stateMux);
    phase = 0;
    amplitudeEnvelope = 1.0f;
    pitchEnvelope = 1.0f;
    clickEnvelope = 1.0f;
    velocityGain = (float)velocity * (1.0f / 127.0f);
    ++triggerGeneration;
    portEXIT_CRITICAL(&stateMux);
}

OSPEED
void AudioSynthKick::update()
{
    uint32_t increment;
    float ampDecay;
    float pitchRatioLocal;
    float pitchDecay;
    float clickAmount;
    float clickDecay;

    portENTER_CRITICAL(&configMux);
    increment = phaseIncrement;
    ampDecay = amplitudeDecay;
    pitchRatioLocal = pitchRatio;
    pitchDecay = pitchDecayCoefficient;
    clickAmount = clickLevel;
    clickDecay = clickDecayCoefficient;
    portEXIT_CRITICAL(&configMux);

    uint32_t localPhase;
    uint32_t generation;
    uint32_t localNoiseState;
    float ampEnv;
    float pitchEnv;
    float clickEnv;
    float velocity;

    portENTER_CRITICAL(&stateMux);
    localPhase = phase;
    ampEnv = amplitudeEnvelope;
    pitchEnv = pitchEnvelope;
    clickEnv = clickEnvelope;
    velocity = velocityGain;
    localNoiseState = noiseState;
    generation = triggerGeneration;
    portEXIT_CRITICAL(&stateMux);

    if (ampEnv <= SILENCE_THRESHOLD &&
        (clickAmount <= 0.0f || clickEnv <= SILENCE_THRESHOLD)) {
        return;
    }

    audio_block_t *block = allocate();

    for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        const float pitch = 1.0f + (pitchRatioLocal - 1.0f) * pitchEnv;
        localPhase += (uint32_t)((float)increment * pitch);

        const float body = sineUnit(localPhase) * ampEnv * 0.92f;
        float transient = 0.0f;
        if (clickAmount > 0.0f && clickEnv > SILENCE_THRESHOLD) {
            transient = noiseUnit(localNoiseState) * clickEnv *
                        clickAmount * 0.28f;
        }

        if (block != nullptr) {
            block->data[i] = toPcm16((body + transient) * velocity);
        }

        ampEnv *= ampDecay;
        pitchEnv *= pitchDecay;
        clickEnv *= clickDecay;

        if (ampEnv < SILENCE_THRESHOLD) ampEnv = 0.0f;
        if (pitchEnv < SILENCE_THRESHOLD) pitchEnv = 0.0f;
        if (clickEnv < SILENCE_THRESHOLD) clickEnv = 0.0f;
    }

    portENTER_CRITICAL(&stateMux);
    if (triggerGeneration == generation) {
        phase = localPhase;
        amplitudeEnvelope = ampEnv;
        pitchEnvelope = pitchEnv;
        clickEnvelope = clickEnv;
        noiseState = localNoiseState;
    }
    portEXIT_CRITICAL(&stateMux);

    if (block != nullptr) {
        transmit(block, 0);
        release(block);
    }
}

// -----------------------------------------------------------------------------
// Tom
// -----------------------------------------------------------------------------

AudioSynthTom::AudioSynthTom() : AudioStream(0, nullptr)
{
    frequency(125.0f);
    length(420.0f);
    pitchSweep(0.26f);
    pitchDecay(42.0f);
    secondMix(0.55f);
    attack(0.22f);
}

void AudioSynthTom::frequency(float frequencyHz)
{
    if (!isfinite(frequencyHz)) return;

    const float fs = sampleRate();
    if (frequencyHz < 35.0f) frequencyHz = 35.0f;

    // The third mode (2.136 x) must remain below Nyquist even at the maximum
    // pitch sweep of 1.5 octaves: 0.07 * 2.136 * 2^1.5 < 0.45.
    if (frequencyHz > fs * 0.07f) frequencyHz = fs * 0.07f;

    const uint32_t increment = phaseIncrementForFrequency(frequencyHz, fs);
    const uint32_t secondIncrement =
        phaseIncrementForFrequency(frequencyHz * 1.593f, fs);
    const uint32_t thirdIncrement =
        phaseIncrementForFrequency(frequencyHz * 2.136f, fs);

    // A broad, band-limited stick transient. Subtracting two low-pass noise
    // signals produces a compact band-pass-like attack without sample data.
    const float fastCoefficient =
        1.0f - expf(-DRUM_TWO_PI * 8500.0f / fs);
    const float slowCoefficient =
        1.0f - expf(-DRUM_TWO_PI * 1200.0f / fs);
    const float attackDecay = timeConstantCoefficient(3.5f, fs);

    portENTER_CRITICAL(&configMux);
    phaseIncrement = increment;
    secondPhaseIncrement = secondIncrement;
    thirdPhaseIncrement = thirdIncrement;
    noiseFastCoefficient = fastCoefficient;
    noiseSlowCoefficient = slowCoefficient;
    attackDecayCoefficient = attackDecay;
    portEXIT_CRITICAL(&configMux);
}

void AudioSynthTom::length(float milliseconds)
{
    if (!isfinite(milliseconds)) return;
    if (milliseconds < 5.0f) milliseconds = 5.0f;
    if (milliseconds > MAX_DECAY_MS) milliseconds = MAX_DECAY_MS;

    // The fundamental carries the body. The higher membrane modes decay
    // progressively faster, as they do on an acoustic drum.
    const float decay1 = amplitudeDecayCoefficient(milliseconds, sampleRate());
    const float decay2 = amplitudeDecayCoefficient(milliseconds * 0.62f, sampleRate());
    const float decay3 = amplitudeDecayCoefficient(milliseconds * 0.38f, sampleRate());

    portENTER_CRITICAL(&configMux);
    amplitudeDecay = decay1;
    secondAmplitudeDecay = decay2;
    thirdAmplitudeDecay = decay3;
    portEXIT_CRITICAL(&configMux);
}

void AudioSynthTom::pitchSweep(float octaves)
{
    if (!isfinite(octaves)) return;
    if (octaves < 0.0f) octaves = 0.0f;
    if (octaves > 1.5f) octaves = 1.5f;

    const float ratio = powf(2.0f, octaves);
    const float secondRatio = powf(2.0f, octaves * 0.78f);
    const float thirdRatio = powf(2.0f, octaves * 0.58f);
    portENTER_CRITICAL(&configMux);
    pitchRatio = ratio;
    secondPitchRatio = secondRatio;
    thirdPitchRatio = thirdRatio;
    portEXIT_CRITICAL(&configMux);
}

void AudioSynthTom::pitchDecay(float milliseconds)
{
    if (!isfinite(milliseconds)) return;
    if (milliseconds < 1.0f) milliseconds = 1.0f;
    if (milliseconds > 500.0f) milliseconds = 500.0f;

    const float coefficient = timeConstantCoefficient(milliseconds, sampleRate());
    portENTER_CRITICAL(&configMux);
    pitchDecayCoefficient = coefficient;
    portEXIT_CRITICAL(&configMux);
}

void AudioSynthTom::secondMix(float level)
{
    if (!isfinite(level)) return;

    level = clampLevel(level);
    portENTER_CRITICAL(&configMux);
    secondMixLevel = level;
    portEXIT_CRITICAL(&configMux);
}

void AudioSynthTom::attack(float level)
{
    if (!isfinite(level)) return;

    level = clampLevel(level);
    portENTER_CRITICAL(&configMux);
    attackLevel = level;
    portEXIT_CRITICAL(&configMux);
}

void AudioSynthTom::noteOn(uint8_t velocity)
{
    if (velocity == 0) return;

    portENTER_CRITICAL(&stateMux);
    phase = 0;
    secondPhase = 0x24000000U;
    thirdPhase = 0x58000000U;
    amplitudeEnvelope = 1.0f;
    secondAmplitudeEnvelope = 1.0f;
    thirdAmplitudeEnvelope = 1.0f;
    pitchEnvelope = 1.0f;
    attackEnvelope = 1.0f;
    noiseFast = 0.0f;
    noiseSlow = 0.0f;
    velocityGain = (float)velocity * (1.0f / 127.0f);
    ++triggerGeneration;
    portEXIT_CRITICAL(&stateMux);
}

OSPEED
void AudioSynthTom::update()
{
    uint32_t increment;
    uint32_t secondIncrement;
    uint32_t thirdIncrement;
    float ampDecay;
    float secondAmpDecay;
    float thirdAmpDecay;
    float pitchRatioLocal;
    float secondPitchRatioLocal;
    float thirdPitchRatioLocal;
    float pitchDecay;
    float secondMixLocal;
    float attackAmount;
    float fastCoefficient;
    float slowCoefficient;
    float attackDecay;

    portENTER_CRITICAL(&configMux);
    increment = phaseIncrement;
    secondIncrement = secondPhaseIncrement;
    thirdIncrement = thirdPhaseIncrement;
    ampDecay = amplitudeDecay;
    secondAmpDecay = secondAmplitudeDecay;
    thirdAmpDecay = thirdAmplitudeDecay;
    pitchRatioLocal = pitchRatio;
    secondPitchRatioLocal = secondPitchRatio;
    thirdPitchRatioLocal = thirdPitchRatio;
    pitchDecay = pitchDecayCoefficient;
    secondMixLocal = secondMixLevel;
    attackAmount = attackLevel;
    fastCoefficient = noiseFastCoefficient;
    slowCoefficient = noiseSlowCoefficient;
    attackDecay = attackDecayCoefficient;
    portEXIT_CRITICAL(&configMux);

    uint32_t localPhase;
    uint32_t localSecondPhase;
    uint32_t localThirdPhase;
    uint32_t localNoiseState;
    uint32_t generation;
    float ampEnv;
    float secondAmpEnv;
    float thirdAmpEnv;
    float pitchEnv;
    float attackEnv;
    float noiseFastLocal;
    float noiseSlowLocal;
    float velocity;

    portENTER_CRITICAL(&stateMux);
    localPhase = phase;
    localSecondPhase = secondPhase;
    localThirdPhase = thirdPhase;
    localNoiseState = noiseState;
    ampEnv = amplitudeEnvelope;
    secondAmpEnv = secondAmplitudeEnvelope;
    thirdAmpEnv = thirdAmplitudeEnvelope;
    pitchEnv = pitchEnvelope;
    attackEnv = attackEnvelope;
    noiseFastLocal = noiseFast;
    noiseSlowLocal = noiseSlow;
    velocity = velocityGain;
    generation = triggerGeneration;
    portEXIT_CRITICAL(&stateMux);

    if (ampEnv <= SILENCE_THRESHOLD &&
        secondAmpEnv <= SILENCE_THRESHOLD &&
        thirdAmpEnv <= SILENCE_THRESHOLD &&
        attackEnv <= SILENCE_THRESHOLD) {
        return;
    }

    audio_block_t *block = allocate();

    // The first three modes of an ideal circular membrane are inharmonic.
    // The upper two modes are quieter and have shorter decay times.
    const float fundamentalGain = 0.82f;
    const float secondGain = 0.24f * secondMixLocal;
    const float thirdGain = 0.12f * secondMixLocal;
    const float outputGain = 0.88f;

    for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        const float pitch1 = 1.0f + (pitchRatioLocal - 1.0f) * pitchEnv;
        const float pitch2 = 1.0f + (secondPitchRatioLocal - 1.0f) * pitchEnv;
        const float pitch3 = 1.0f + (thirdPitchRatioLocal - 1.0f) * pitchEnv;

        localPhase += (uint32_t)((float)increment * pitch1);
        localSecondPhase += (uint32_t)((float)secondIncrement * pitch2);
        localThirdPhase += (uint32_t)((float)thirdIncrement * pitch3);

        const float fundamental = sineUnit(localPhase) * ampEnv * fundamentalGain;
        const float mode2 = sineUnit(localSecondPhase) * secondAmpEnv * secondGain;
        const float mode3 = sineUnit(localThirdPhase) * thirdAmpEnv * thirdGain;

        const float white = noiseUnit(localNoiseState);
        noiseFastLocal += fastCoefficient * (white - noiseFastLocal);
        noiseSlowLocal += slowCoefficient * (white - noiseSlowLocal);
        const float stickNoise = (noiseFastLocal - noiseSlowLocal) *
                                 attackEnv * attackAmount * 0.42f;

        const float sample = ((fundamental + mode2 + mode3) * outputGain +
                              stickNoise) * velocity;

        if (block != nullptr) {
            block->data[i] = toPcm16(sample);
        }

        ampEnv *= ampDecay;
        secondAmpEnv *= secondAmpDecay;
        thirdAmpEnv *= thirdAmpDecay;
        pitchEnv *= pitchDecay;
        attackEnv *= attackDecay;

        if (ampEnv < SILENCE_THRESHOLD) ampEnv = 0.0f;
        if (secondAmpEnv < SILENCE_THRESHOLD) secondAmpEnv = 0.0f;
        if (thirdAmpEnv < SILENCE_THRESHOLD) thirdAmpEnv = 0.0f;
        if (pitchEnv < SILENCE_THRESHOLD) pitchEnv = 0.0f;
        if (attackEnv < SILENCE_THRESHOLD) attackEnv = 0.0f;
    }

    portENTER_CRITICAL(&stateMux);
    if (triggerGeneration == generation) {
        phase = localPhase;
        secondPhase = localSecondPhase;
        thirdPhase = localThirdPhase;
        noiseState = localNoiseState;
        amplitudeEnvelope = ampEnv;
        secondAmplitudeEnvelope = secondAmpEnv;
        thirdAmplitudeEnvelope = thirdAmpEnv;
        pitchEnvelope = pitchEnv;
        attackEnvelope = attackEnv;
        noiseFast = noiseFastLocal;
        noiseSlow = noiseSlowLocal;
    }
    portEXIT_CRITICAL(&stateMux);

    if (block != nullptr) {
        transmit(block, 0);
        release(block);
    }
}

// -----------------------------------------------------------------------------
// Snare
// -----------------------------------------------------------------------------

AudioSynthSnare::AudioSynthSnare() : AudioStream(0, nullptr)
{
    frequency(185.0f);
    length(190.0f);
    snappy(0.85f);
    tone(0.62f);
}

void AudioSynthSnare::frequency(float frequencyHz)
{
    if (!isfinite(frequencyHz)) return;

    const float fs = sampleRate();
    if (frequencyHz < 70.0f) frequencyHz = 70.0f;
    if (frequencyHz > fs * 0.20f) frequencyHz = fs * 0.20f;

    const uint32_t increment = phaseIncrementForFrequency(frequencyHz, fs);
    const uint32_t secondIncrement =
        phaseIncrementForFrequency(frequencyHz * 1.78f, fs);
    const float fastCoefficient = 1.0f - expf(-DRUM_TWO_PI * 7200.0f / fs);
    const float slowCoefficient = 1.0f - expf(-DRUM_TWO_PI * 950.0f / fs);

    portENTER_CRITICAL(&configMux);
    phaseIncrement = increment;
    secondPhaseIncrement = secondIncrement;
    noiseFastCoefficient = fastCoefficient;
    noiseSlowCoefficient = slowCoefficient;
    portEXIT_CRITICAL(&configMux);
}

void AudioSynthSnare::length(float milliseconds)
{
    if (!isfinite(milliseconds)) return;
    if (milliseconds < 5.0f) milliseconds = 5.0f;
    if (milliseconds > MAX_DECAY_MS) milliseconds = MAX_DECAY_MS;

    // Keep the snare-wire noise present through most of the shell decay.
    // The slightly shorter noise tail still lets the shell define the total length.
    const float bodyCoefficient =
        amplitudeDecayCoefficient(milliseconds * 1.15f, sampleRate());
    const float noiseCoefficient =
        amplitudeDecayCoefficient(milliseconds * 1.00f, sampleRate());

    portENTER_CRITICAL(&configMux);
    bodyDecay = bodyCoefficient;
    noiseDecay = noiseCoefficient;
    portEXIT_CRITICAL(&configMux);
}

void AudioSynthSnare::snappy(float level)
{
    if (!isfinite(level)) return;

    level = clampLevel(level);
    portENTER_CRITICAL(&configMux);
    snappyLevel = level;
    portEXIT_CRITICAL(&configMux);
}

void AudioSynthSnare::tone(float level)
{
    if (!isfinite(level)) return;

    level = clampLevel(level);
    portENTER_CRITICAL(&configMux);
    toneLevel = level;
    portEXIT_CRITICAL(&configMux);
}

void AudioSynthSnare::noteOn(uint8_t velocity)
{
    if (velocity == 0) return;

    portENTER_CRITICAL(&stateMux);
    phase = 0;
    secondPhase = 0;
    bodyEnvelope = 1.0f;
    noiseEnvelope = 1.0f;
    noiseFast = 0.0f;
    noiseSlow = 0.0f;
    velocityGain = (float)velocity * (1.0f / 127.0f);
    ++triggerGeneration;
    portEXIT_CRITICAL(&stateMux);
}

OSPEED
void AudioSynthSnare::update()
{
    uint32_t increment;
    uint32_t secondIncrement;
    float bodyDecayLocal;
    float noiseDecayLocal;
    float fastCoefficient;
    float slowCoefficient;
    float snappyLocal;
    float toneLocal;

    portENTER_CRITICAL(&configMux);
    increment = phaseIncrement;
    secondIncrement = secondPhaseIncrement;
    bodyDecayLocal = bodyDecay;
    noiseDecayLocal = noiseDecay;
    fastCoefficient = noiseFastCoefficient;
    slowCoefficient = noiseSlowCoefficient;
    snappyLocal = snappyLevel;
    toneLocal = toneLevel;
    portEXIT_CRITICAL(&configMux);

    uint32_t localPhase;
    uint32_t localSecondPhase;
    uint32_t localNoiseState;
    uint32_t generation;
    float bodyEnv;
    float noiseEnv;
    float noiseFastLocal;
    float noiseSlowLocal;
    float velocity;

    portENTER_CRITICAL(&stateMux);
    localPhase = phase;
    localSecondPhase = secondPhase;
    localNoiseState = noiseState;
    bodyEnv = bodyEnvelope;
    noiseEnv = noiseEnvelope;
    noiseFastLocal = noiseFast;
    noiseSlowLocal = noiseSlow;
    velocity = velocityGain;
    generation = triggerGeneration;
    portEXIT_CRITICAL(&stateMux);

    if (bodyEnv <= SILENCE_THRESHOLD && noiseEnv <= SILENCE_THRESHOLD) return;

    audio_block_t *block = allocate();

    for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        const float white = noiseUnit(localNoiseState);
        noiseFastLocal += fastCoefficient * (white - noiseFastLocal);
        noiseSlowLocal += slowCoefficient * (white - noiseSlowLocal);
        const float bandNoise = noiseFastLocal - noiseSlowLocal;

        localPhase += increment;
        localSecondPhase += secondIncrement;

        const float shell = (sineUnit(localPhase) * 0.72f +
                             sineUnit(localSecondPhase) * 0.28f) *
                            bodyEnv * toneLocal;
        const float wires = bandNoise * noiseEnv * snappyLocal;
        const float sample = (shell * 0.58f + wires * 1.55f) * velocity;

        if (block != nullptr) {
            block->data[i] = toPcm16(sample);
        }

        bodyEnv *= bodyDecayLocal;
        noiseEnv *= noiseDecayLocal;
        if (bodyEnv < SILENCE_THRESHOLD) bodyEnv = 0.0f;
        if (noiseEnv < SILENCE_THRESHOLD) noiseEnv = 0.0f;
    }

    portENTER_CRITICAL(&stateMux);
    if (triggerGeneration == generation) {
        phase = localPhase;
        secondPhase = localSecondPhase;
        noiseState = localNoiseState;
        bodyEnvelope = bodyEnv;
        noiseEnvelope = noiseEnv;
        noiseFast = noiseFastLocal;
        noiseSlow = noiseSlowLocal;
    }
    portEXIT_CRITICAL(&stateMux);

    if (block != nullptr) {
        transmit(block, 0);
        release(block);
    }
}
