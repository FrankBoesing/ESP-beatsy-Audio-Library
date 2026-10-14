/*
 * Wavetable Drum Computer
 *
 * Four-bar intro followed by an original, looping alternative-rock groove.
 * The arrangement is inspired by the punchy 1990s alt-rock feel of drum-led
 * intros; it is not a note-for-note transcription of a released recording.
 *
 * The kick, snare, hi-hats and toms use short one-shot samples prepared from
 * Aspirin DX Soundbank - Fairytale Edition (CC0 1.0). AudioSynthWavetable
 * handles sample interpolation and velocity; the sequencer remains non-blocking.
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

// Each sample player is a separate AudioStream node. One-shot samples retain
// their recorded transient and decay; the kick additionally gets a short pitch drop.
AudioSynthWavetable kick;
AudioSynthWavetable snare;
AudioSynthWavetable highTom;
AudioSynthWavetable lowTom;
AudioSynthWavetable closedHat;
AudioSynthWavetable openHat;

AudioMixer4 drumMixer;
AudioMixer4 hatMixer;
AudioMixer4 masterMixer;

AudioConnection kickToDrums(kick, 0, drumMixer, 0);
AudioConnection snareToDrums(snare, 0, drumMixer, 1);
AudioConnection lowTomToDrums(lowTom, 0, drumMixer, 2);
AudioConnection highTomToDrums(highTom, 0, drumMixer, 3);
AudioConnection closedHatToMix(closedHat, 0, hatMixer, 0);
AudioConnection openHatToMix(openHat, 0, hatMixer, 1);
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
// with a descending tom fill so each pass leads naturally into the next downbeat.
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
uint32_t kickSweepStarted = 0;
uint32_t kickSweepLastUpdate = UINT32_MAX;
bool kickSweepActive = false;

bool timeReached(uint32_t now, uint32_t deadline)
{
    return static_cast<int32_t>(now - deadline) >= 0;
}

void triggerKick(uint32_t now, int velocity)
{
    kick.playFrequency(142.0f, velocity);
    kickSweepStarted = now;
    kickSweepLastUpdate = now - 1U;
    kickSweepActive = true;
}

void updateKickPitch(uint32_t now)
{
    // Avoid repeated frequency changes within the same millisecond.
    if (!kickSweepActive || now == kickSweepLastUpdate) {
        return;
    }
    kickSweepLastUpdate = now;

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
        // The small pitch lift on the fill makes the final snare hit stand out.
        snare.playFrequency((stepInBar == 11U) ? 205.0f : 178.0f, velocity);
    }

    if ((tomPattern & hit) != 0U) {
        const int velocity = (stepInBar >= 14U) ? 108 : 90;
        // High toms first, then the lower tom at the end of the fill.
        if (stepInBar < 14U) {
            highTom.playFrequency(118.0f, velocity);
        } else {
            lowTom.playFrequency(91.0f, velocity);
        }
    }

    // Eighth-note sampled hi-hats. An open hat lifts the end of each phrase.
    if ((stepInBar % 2U) == 0U) {
        if (stepInBar == 14U && phraseBar == 3U) {
            openHat.playFrequency(440.0f, 112);
        } else {
            const int velocity = (stepInBar % 4U) == 0U ? 83 : 67;
            closedHat.playFrequency(440.0f, velocity);
        }
    } else if (stepInBar == 7U && (phraseBar == 1U || phraseBar == 3U)) {
        // Quiet sixteenth-note ghost hats add a little movement.
        closedHat.playFrequency(440.0f, 48);
    }

    ++stepCounter;

    if (stepCounter == INTRO_STEPS) {
        Serial.println("Intro complete: continuing with the looping rock groove.");
    }
}

} // namespace

void setup()
{
    Serial.begin(115200);
    Serial.println("Wavetable Drum Computer");

    AudioMemory(40);
    AudioNoInterrupts();

    kick.setInstrument(RockDrumTables::kickInstrument);
    snare.setInstrument(RockDrumTables::snareInstrument);
    highTom.setInstrument(RockDrumTables::highTomInstrument);
    lowTom.setInstrument(RockDrumTables::lowTomInstrument);
    closedHat.setInstrument(RockDrumTables::closedHatInstrument);
    openHat.setInstrument(RockDrumTables::openHatInstrument);

    drumMixer.gain(0, 0.72f); // kick
    drumMixer.gain(1, 0.64f); // snare
    drumMixer.gain(2, 0.44f); // low tom
    drumMixer.gain(3, 0.44f); // high tom

    hatMixer.gain(0, 0.30f); // closed hat
    hatMixer.gain(1, 0.28f); // open hat

    masterMixer.gain(0, 0.82f);
    masterMixer.gain(1, 0.66f);

    if (!codec.enable()) {
        Serial.println("ERROR: audio codec initialization failed");
        AudioInterrupts();
        while (true) {
            delay(1000);
        }
    }
    codec.volume(0.75f);

    AudioInterrupts();

    nextStepAt = millis() + 250U;
    Serial.println("Playing four-bar intro, then an endless rock groove.");
}

void loop()
{
    const uint32_t now = millis();

    updateKickPitch(now);

    if (timeReached(now, nextStepAt)) {
        // Schedule from the current time to avoid bursts after a delayed loop.
        nextStepAt = now + STEP_MS;
        playStep(now);
    }
}
