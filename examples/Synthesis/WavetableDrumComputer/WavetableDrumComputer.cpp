/*
 * Wavetable Drum Computer
 *
 * Four-bar intro followed by an original, looping alternative-rock groove.
 * The arrangement is inspired by the punchy 1990s alt-rock feel of drum-led
 * intros; it is not a note-for-note transcription of a released recording.
 *
 * Kick, snare body and tom use AudioSynthWavetable with compact single-cycle
 * tables. A shared white-noise source adds the snare snap and closed/open hats.
 * The sequencer and its note-off / kick-pitch updates are non-blocking.
 */

#include <Audio.h>
#include <stdint.h>

#include "WavetableDrumTables.h"

AudioOutputI2S audioOut({
    PIN_I2S_BLCK, // BCLK
    PIN_I2S_WS,   // WS / LRCLK
    PIN_I2S_DOUT, // DOUT
    PIN_I2S_MLCK  // MCLK
});

#if defined(AUDIO_CODEC_ES8388)
AudioControlES8388 codec({PIN_I2C_SDA, PIN_I2C_SCL, PIN_AMPLIFIER, PIN_AMPLIFIER_ACTIVE});
#elif defined(AUDIO_CODEC_ES8311)
AudioControlES8311 codec({PIN_I2C_SDA, PIN_I2C_SCL, PIN_AMPLIFIER, PIN_AMPLIFIER_ACTIVE});
#else
#error "AUDIO_CODEC not defined"
#endif

// Wavetable voices: tonal drum bodies with their own short envelopes.
AudioSynthWavetable kick;
AudioSynthWavetable snareBody;
AudioSynthWavetable tom;

// One noise source can feed independent envelopes for simultaneous hits.
AudioSynthNoiseWhite percussionNoise;
AudioEffectEnvelope snareNoiseEnvelope;
AudioEffectEnvelope closedHatEnvelope;
AudioEffectEnvelope openHatEnvelope;

AudioMixer4 drumMixer;
AudioMixer4 hatMixer;
AudioMixer4 masterMixer;

AudioConnection kickToDrums(kick, 0, drumMixer, 0);
AudioConnection snareBodyToDrums(snareBody, 0, drumMixer, 1);
AudioConnection tomToDrums(tom, 0, drumMixer, 2);
AudioConnection noiseToSnare(percussionNoise, snareNoiseEnvelope);
AudioConnection snareNoiseToDrums(snareNoiseEnvelope, 0, drumMixer, 3);
AudioConnection noiseToClosedHat(percussionNoise, closedHatEnvelope);
AudioConnection noiseToOpenHat(percussionNoise, openHatEnvelope);
AudioConnection closedHatToMix(closedHatEnvelope, 0, hatMixer, 0);
AudioConnection openHatToMix(openHatEnvelope, 0, hatMixer, 1);
AudioConnection drumsToMaster(drumMixer, 0, masterMixer, 0);
AudioConnection hatsToMaster(hatMixer, 0, masterMixer, 1);
AudioConnection masterToLeft(masterMixer, 0, audioOut, 0);
AudioConnection masterToRight(masterMixer, 0, audioOut, 1);

namespace {

constexpr uint32_t BPM = 107;
constexpr uint32_t STEP_MS = 60000UL / (BPM * 4U); // sixteenth notes
constexpr uint8_t STEPS_PER_BAR = 16;
constexpr uint32_t INTRO_STEPS = 4U * STEPS_PER_BAR;

constexpr uint16_t stepBit(uint8_t step)
{
    return static_cast<uint16_t>(1U << step);
}

constexpr uint16_t INTRO_KICK[4] = {
    stepBit(0) | stepBit(6) | stepBit(8) | stepBit(14),
    stepBit(0) | stepBit(7) | stepBit(8) | stepBit(11) | stepBit(14),
    stepBit(0) | stepBit(6) | stepBit(8) | stepBit(10) | stepBit(14),
    stepBit(0) | stepBit(5) | stepBit(8) | stepBit(10) | stepBit(14) | stepBit(15)
};

constexpr uint16_t INTRO_SNARE[4] = {
    stepBit(4) | stepBit(12),
    stepBit(4) | stepBit(12),
    stepBit(4) | stepBit(12),
    stepBit(4) | stepBit(11) | stepBit(12)
};

constexpr uint16_t INTRO_TOM[4] = {
    0U,
    0U,
    0U,
    stepBit(11) | stepBit(13) | stepBit(14) | stepBit(15)
};

// The main groove is four bars long and repeats indefinitely. Bar four ends
// with a tom fill so every pass leads naturally into the next downbeat.
constexpr uint16_t GROOVE_KICK[4] = {
    stepBit(0) | stepBit(7) | stepBit(8) | stepBit(11) | stepBit(14),
    stepBit(0) | stepBit(6) | stepBit(8) | stepBit(10) | stepBit(14),
    stepBit(0) | stepBit(5) | stepBit(8) | stepBit(10) | stepBit(14),
    stepBit(0) | stepBit(6) | stepBit(8) | stepBit(10) | stepBit(14) | stepBit(15)
};

constexpr uint16_t GROOVE_SNARE[4] = {
    stepBit(4) | stepBit(12),
    stepBit(4) | stepBit(12),
    stepBit(4) | stepBit(12),
    stepBit(4) | stepBit(11)
};

constexpr uint16_t GROOVE_TOM[4] = {
    0U,
    0U,
    0U,
    stepBit(12) | stepBit(13) | stepBit(14) | stepBit(15)
};

uint32_t nextStepAt = 0;
uint32_t stepCounter = 0;

uint32_t snareOffAt = 0;
uint32_t closedHatOffAt = 0;
uint32_t openHatOffAt = 0;
bool snareActive = false;
bool closedHatActive = false;
bool openHatActive = false;

uint32_t kickSweepStarted = 0;
bool kickSweepActive = false;

bool timeReached(uint32_t now, uint32_t deadline)
{
    return static_cast<int32_t>(now - deadline) >= 0;
}

void triggerEnvelope(AudioEffectEnvelope &envelope, bool &active,
                    uint32_t &noteOffAt, uint32_t now, uint32_t gateMs)
{
    envelope.noteOn();
    active = true;
    noteOffAt = now + gateMs;
}

void triggerKick(uint32_t now, int velocity)
{
    kick.playFrequency(142.0f, velocity);
    kickSweepStarted = now;
    kickSweepActive = true;
}

void updateKickPitch(uint32_t now)
{
    if (!kickSweepActive) {
        return;
    }

    const uint32_t elapsed = now - kickSweepStarted;
    constexpr uint32_t SWEEP_MS = 65U;
    if (elapsed >= SWEEP_MS) {
        kick.setFrequency(48.0f);
        kickSweepActive = false;
        return;
    }

    const float progress = static_cast<float>(elapsed) / static_cast<float>(SWEEP_MS);
    const float curve = (1.0f - progress) * (1.0f - progress);
    kick.setFrequency(48.0f + 94.0f * curve);
}

void playStep(uint32_t now)
{
    const uint8_t stepInBar = static_cast<uint8_t>(stepCounter % STEPS_PER_BAR);
    const uint32_t barNumber = stepCounter / STEPS_PER_BAR;
    const bool intro = barNumber < 4U;
    const uint8_t phraseBar = intro
        ? static_cast<uint8_t>(barNumber)
        : static_cast<uint8_t>((barNumber - 4U) % 4U);

    const uint16_t kickPattern = intro ? INTRO_KICK[phraseBar] : GROOVE_KICK[phraseBar];
    const uint16_t snarePattern = intro ? INTRO_SNARE[phraseBar] : GROOVE_SNARE[phraseBar];
    const uint16_t tomPattern = intro ? INTRO_TOM[phraseBar] : GROOVE_TOM[phraseBar];
    const uint16_t hit = stepBit(stepInBar);

    if ((kickPattern & hit) != 0U) {
        const int velocity = (stepInBar == 0U || stepInBar == 8U) ? 112 : 94;
        triggerKick(now, velocity);
    }

    if ((snarePattern & hit) != 0U) {
        const int velocity = (stepInBar == 4U) ? 103 : 112;
        snareBody.playFrequency((stepInBar == 11U) ? 205.0f : 178.0f, velocity);
        triggerEnvelope(snareNoiseEnvelope, snareActive, snareOffAt, now, 18U);
    }

    if ((tomPattern & hit) != 0U) {
        const float frequency = (stepInBar < 13U) ? 118.0f : 91.0f;
        const int velocity = (stepInBar >= 14U) ? 105 : 86;
        tom.playFrequency(frequency, velocity);
    }

    // Eighth-note hats, with an open-hat lift at the end of each four-bar phrase.
    if ((stepInBar % 2U) == 0U) {
        if (stepInBar == 14U && phraseBar == 3U) {
            triggerEnvelope(openHatEnvelope, openHatActive, openHatOffAt, now, 65U);
        } else {
            triggerEnvelope(closedHatEnvelope, closedHatActive, closedHatOffAt, now, 12U);
        }
    } else if (stepInBar == 7U && (phraseBar == 1U || phraseBar == 3U)) {
        // Quiet sixteenth-note ghost hats help the repeating groove breathe.
        triggerEnvelope(closedHatEnvelope, closedHatActive, closedHatOffAt, now, 8U);
    }

    ++stepCounter;

    if (stepCounter == INTRO_STEPS) {
        Serial.println("Intro complete: continuing with the looping rock groove.");
    }
}

void processNoteOffs(uint32_t now)
{
    if (snareActive && timeReached(now, snareOffAt)) {
        snareNoiseEnvelope.noteOff();
        snareActive = false;
    }

    if (closedHatActive && timeReached(now, closedHatOffAt)) {
        closedHatEnvelope.noteOff();
        closedHatActive = false;
    }

    if (openHatActive && timeReached(now, openHatOffAt)) {
        openHatEnvelope.noteOff();
        openHatActive = false;
    }
}

} // namespace

void setup()
{
    Serial.begin(115200);
    Serial.println("Wavetable Drum Computer");

    RockDrumTables::initialize();
    AudioMemory(32);

    AudioNoInterrupts();

    kick.setInstrument(RockDrumTables::kickInstrument);
    snareBody.setInstrument(RockDrumTables::snareInstrument);
    tom.setInstrument(RockDrumTables::tomInstrument);

    percussionNoise.amplitude(0.42f);

    snareNoiseEnvelope.attack(0.4f);
    snareNoiseEnvelope.hold(1.5f);
    snareNoiseEnvelope.decay(18.0f);
    snareNoiseEnvelope.sustain(0.02f);
    snareNoiseEnvelope.release(22.0f);

    closedHatEnvelope.attack(0.3f);
    closedHatEnvelope.hold(0.0f);
    closedHatEnvelope.decay(8.0f);
    closedHatEnvelope.sustain(0.0f);
    closedHatEnvelope.release(8.0f);

    openHatEnvelope.attack(0.4f);
    openHatEnvelope.hold(1.0f);
    openHatEnvelope.decay(42.0f);
    openHatEnvelope.sustain(0.03f);
    openHatEnvelope.release(34.0f);

    drumMixer.gain(0, 0.78f); // kick
    drumMixer.gain(1, 0.32f); // snare wavetable body
    drumMixer.gain(2, 0.33f); // tom
    drumMixer.gain(3, 0.28f); // snare noise

    hatMixer.gain(0, 0.24f); // closed hat
    hatMixer.gain(1, 0.28f); // open hat

    masterMixer.gain(0, 0.88f);
    masterMixer.gain(1, 0.64f);

    if (!codec.enable()) {
        Serial.println("ERROR: audio codec initialization failed");
        AudioInterrupts();
        return;
    }
    codec.volume(0.75f);

    AudioInterrupts();

    nextStepAt = millis() + 250U;
    Serial.println("Playing four-bar intro, then an endless rock groove.");
}

void loop()
{
    const uint32_t now = millis();

    processNoteOffs(now);
    updateKickPitch(now);

    if (timeReached(now, nextStepAt)) {
        nextStepAt = now + STEP_MS;
        playStep(now);
    }
}
