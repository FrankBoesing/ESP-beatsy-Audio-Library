/*
 * Analog Drum Kit
 *
 * A self-running four-bar groove demonstrating AudioSynthKick,
 * AudioSynthTom and AudioSynthSnare. The pattern adds small timing-free
 * variations, ghost notes and a tom fill at the end of each four-bar phrase.
 *
 * Board setup (codec and I2S pins) is supplied by platformio.ini.
 */

#include <Audio.h>
#include <esp_system.h>

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
#error "Select AUDIO_CODEC_ES8388 or AUDIO_CODEC_ES8311 in build_src_flags"
#endif

// -----------------------------------------------------------------------------
// Drum voices
// -----------------------------------------------------------------------------

AudioSynthKick kick;
AudioSynthTom lowTom;
AudioSynthTom highTom;
AudioSynthSnare snare;

// Separate noise sources keep an open-hat tail independent of later closed hats.
AudioSynthNoiseWhite closedHatNoise;
AudioSynthNoiseWhite openHatNoise;
AudioEffectEnvelope closedHatEnvelope;
AudioEffectEnvelope openHatEnvelope;

// -----------------------------------------------------------------------------
// Mix and output
// -----------------------------------------------------------------------------

AudioMixer4 drumMixer;
AudioMixer4 leftMixer;
AudioMixer4 rightMixer;

AudioConnection kickToDrums(kick, 0, drumMixer, 0);
AudioConnection snareToDrums(snare, 0, drumMixer, 1);
AudioConnection lowTomToDrums(lowTom, 0, drumMixer, 2);
AudioConnection highTomToDrums(highTom, 0, drumMixer, 3);

AudioConnection noiseToClosedHat(closedHatNoise, closedHatEnvelope);
AudioConnection noiseToOpenHat(openHatNoise, openHatEnvelope);

AudioConnection drumsToLeft(drumMixer, 0, leftMixer, 0);
AudioConnection drumsToRight(drumMixer, 0, rightMixer, 0);
AudioConnection closedHatToLeft(closedHatEnvelope, 0, leftMixer, 1);
AudioConnection closedHatToRight(closedHatEnvelope, 0, rightMixer, 1);
AudioConnection openHatToLeft(openHatEnvelope, 0, leftMixer, 2);
AudioConnection openHatToRight(openHatEnvelope, 0, rightMixer, 2);

AudioConnection leftToOutput(leftMixer, 0, audioOut, 0);
AudioConnection rightToOutput(rightMixer, 0, audioOut, 1);


constexpr uint32_t BPM = 118;
constexpr uint32_t STEP_MS = 60000UL / (BPM * 4U);
constexpr uint8_t STEPS_PER_BAR = 16;
constexpr uint8_t BARS_PER_PHRASE = 4;

uint32_t nextStepAt = 0;
uint32_t nextStatsAt = 0;
uint32_t stepCounter = 0;

uint32_t closedHatOffAt = 0;
uint32_t openHatOffAt = 0;
bool closedHatActive = false;
bool openHatActive = false;

bool timeReached(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

void triggerClosedHat(uint8_t velocity, uint32_t now)
{
    closedHatNoise.amplitude(0.30f * ((float)velocity / 127.0f));
    closedHatEnvelope.noteOn();
    closedHatOffAt = now + 17;
    closedHatActive = true;
}

void triggerOpenHat(uint8_t velocity, uint32_t now)
{
    openHatNoise.amplitude(0.27f * ((float)velocity / 127.0f));
    openHatEnvelope.noteOn();
    openHatOffAt = now + 34;
    openHatActive = true;
}

void processHatNoteOffs(uint32_t now)
{
    if (closedHatActive && timeReached(now, closedHatOffAt)) {
        closedHatEnvelope.noteOff();
        closedHatActive = false;
    }

    if (openHatActive && timeReached(now, openHatOffAt)) {
        openHatEnvelope.noteOff();
        openHatActive = false;
    }
}

void playStep(uint32_t now)
{
    const uint8_t step = (uint8_t)(stepCounter % STEPS_PER_BAR);
    const uint8_t bar = (uint8_t)((stepCounter / STEPS_PER_BAR) % BARS_PER_PHRASE);
    const bool fillBar = (bar == BARS_PER_PHRASE - 1U);

    // Solid four-on-the-floor foundation, with an occasional syncopated pickup.
    if ((step % 4U) == 0U) {
        const uint8_t velocity = (step == 0U) ? 118 : 108;
        kick.noteOn(velocity);
    } else if ((bar == 1U || bar == 3U) && step == 11U) {
        kick.noteOn(66);
    }

    // Backbeat on beats 2 and 4. A quiet ghost note leads into the phrase fill.
    if (step == 4U || step == 12U) {
        snare.noteOn((step == 4U) ? 112 : 118);
    } else if (step == 10U && (bar == 1U || bar == 3U)) {
        snare.noteOn(42);
    }

    // Eighth-note hats. Open hats lift the second half of alternating bars.
    if ((step % 2U) == 0U) {
        if (step == 14U && (bar == 1U || bar == 3U)) {
            triggerOpenHat(92, now);
        } else {
            const uint8_t velocity = (step % 4U) == 0U ? 82 : 58;
            triggerClosedHat(velocity, now);
        }
    } else if ((step == 7U || step == 15U) && random(100) < 22) {
        triggerClosedHat(35, now);
    }

    // A short, alternating tom run closes every fourth bar.
    if (fillBar && step >= 12U) {
        if (step == 12U) {
            lowTom.noteOn(92);
        } else if (step == 13U) {
            highTom.noteOn(82);
        } else if (step == 14U) {
            lowTom.noteOn(108);
        } else {
            highTom.noteOn(116);
        }
    } else if (step == 8U && bar == 1U) {
        // A single low tom accent adds movement halfway through the phrase.
        lowTom.noteOn(66);
    }

    ++stepCounter;
}

void printStatistics()
{
    Serial.print("Audio CPU: ");
    Serial.print(AudioProcessorUsage(), 1);
    Serial.print("% (max ");
    Serial.print(AudioProcessorUsageMax(), 1);
    Serial.print("%), blocks: ");
    Serial.print(AudioMemoryUsage());
    Serial.print(" (max ");
    Serial.print(AudioMemoryUsageMax());
    Serial.println(")");

    AudioProcessorUsageMaxReset();
    AudioMemoryUsageMaxReset();
}


void setup()
{
    Serial.begin(115200);
    delay(200);

    audioOut.setChannelCount(codec.channels());

    AudioMemory(24);

    // Kick: short, assertive pitch drop with a subtle beater transient.
    kick.frequency(52.0f);
    kick.length(260.0f);
    kick.pitchSweep(2.2f);
    kick.pitchDecay(24.0f);
    kick.click(0.20f);

    // Two tom registers. The modal partials and short stick transient add the
    // characteristic membrane/skin response.
    lowTom.frequency(96.0f);
    lowTom.length(360.0f);
    lowTom.pitchSweep(0.38f);
    lowTom.pitchDecay(58.0f);
    lowTom.secondMix(0.52f);
    lowTom.attack(0.26f);

    highTom.frequency(148.0f);
    highTom.length(285.0f);
    highTom.pitchSweep(0.32f);
    highTom.pitchDecay(48.0f);
    highTom.secondMix(0.46f);
    highTom.attack(0.19f);

    // A balanced snare: crisp noise with a little tuned shell underneath.
    snare.frequency(192.0f);
    snare.length(185.0f);
    snare.snappy(0.81f); // Snare-wire/noise balance.
    snare.tone(0.58f);


    closedHatEnvelope.attack(0.2f);
    closedHatEnvelope.hold(0.0f);
    closedHatEnvelope.decay(10.0f);
    closedHatEnvelope.sustain(0.0f);
    closedHatEnvelope.release(4.0f);

    openHatEnvelope.attack(0.3f);
    openHatEnvelope.hold(18.0f);
    openHatEnvelope.decay(54.0f);
    openHatEnvelope.sustain(0.015f);
    openHatEnvelope.release(48.0f);

    // Leave enough headroom for simultaneous kick + snare hits.
    drumMixer.gain(0, 0.62f);
    drumMixer.gain(1, 0.48f);
    drumMixer.gain(2, 0.48f);
    drumMixer.gain(3, 0.44f);

    leftMixer.gain(0, 0.92f);
    leftMixer.gain(1, 0.23f);
    leftMixer.gain(2, 0.18f);
    rightMixer.gain(0, 0.92f);
    rightMixer.gain(1, 0.18f);
    rightMixer.gain(2, 0.23f);

    if (!codec.enable()) {
        Serial.println("ERROR: audio codec initialization failed");
        while (true) {
            delay(1000);
        }
    }

    codec.volume(0.20f);
    randomSeed(esp_random());

    nextStepAt = millis() + 250;
    nextStatsAt = millis() + 5000;

    Serial.println("Analog Drum Kit started");
    Serial.println("118 BPM | kick + snare backbeat | four-bar tom fills");
}

void loop()
{
    const uint32_t now = millis();

    processHatNoteOffs(now);

    if (timeReached(now, nextStepAt)) {
        nextStepAt += STEP_MS;

        // Do not replay a backlog if setup/serial output temporarily blocks loop().
        if (timeReached(now, nextStepAt)) {
            nextStepAt = now + STEP_MS;
        }

        playStep(now);
    }

    if (timeReached(now, nextStatsAt)) {
        nextStatsAt = now + 5000;
        printStatistics();
    }

    delay(1);
}
