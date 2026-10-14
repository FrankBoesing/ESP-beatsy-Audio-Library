/*
 * Deep House Groove
 *
 * A generative four-on-the-floor groove with a syncopated bass line,
 * chord stabs, clap and closed/open hi-hats. The chord progression stays steady
 * while occasional ghost hi-hats add small rhythmic variations.
 */

#include <Audio.h>
#include <math.h>
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
#error "AUDIO_CODEC not defined"
#endif

// -----------------------------------------------------------------------------
// Drums
// -----------------------------------------------------------------------------

AudioSynthSimpleDrum kick;
AudioSynthNoiseWhite percussionNoise;
AudioEffectEnvelope clapEnvelope;
AudioEffectEnvelope closedHatEnvelope;
AudioEffectEnvelope openHatEnvelope;
AudioMixer4 drumMixer;

// -----------------------------------------------------------------------------
// Bass
// -----------------------------------------------------------------------------

AudioSynthWaveform bass;
AudioFilterBiquad bassFilter;
AudioEffectEnvelope bassEnvelope;

// -----------------------------------------------------------------------------
// Chord stabs
// -----------------------------------------------------------------------------

AudioSynthWaveform chordRoot;
AudioSynthWaveform chordThird;
AudioSynthWaveform chordFifth;
AudioSynthWaveform chordSeventh;
AudioMixer4 chordMixer;
AudioFilterBiquad chordFilter;
AudioEffectEnvelope chordEnvelope;
AudioEffectFreeverbStereo chordReverb;

// -----------------------------------------------------------------------------
// Stereo output mix
// -----------------------------------------------------------------------------

AudioMixer4 leftMixer;
AudioMixer4 rightMixer;

// Drum routing
AudioConnection kickToDrums(kick, 0, drumMixer, 0);
AudioConnection noiseToClap(percussionNoise, clapEnvelope);
AudioConnection noiseToClosedHat(percussionNoise, closedHatEnvelope);
AudioConnection noiseToOpenHat(percussionNoise, openHatEnvelope);
AudioConnection clapToDrums(clapEnvelope, 0, drumMixer, 1);
AudioConnection closedHatToDrums(closedHatEnvelope, 0, drumMixer, 2);
AudioConnection openHatToDrums(openHatEnvelope, 0, drumMixer, 3);

// Bass routing
AudioConnection bassToFilter(bass, bassFilter);
AudioConnection bassToEnvelope(bassFilter, bassEnvelope);

// Chord routing
AudioConnection rootToChord(chordRoot, 0, chordMixer, 0);
AudioConnection thirdToChord(chordThird, 0, chordMixer, 1);
AudioConnection fifthToChord(chordFifth, 0, chordMixer, 2);
AudioConnection seventhToChord(chordSeventh, 0, chordMixer, 3);
AudioConnection chordToFilter(chordMixer, chordFilter);
AudioConnection chordToEnvelope(chordFilter, chordEnvelope);
AudioConnection chordToReverb(chordEnvelope, chordReverb);

// Main stereo routing: drums, bass and chord stabs are centred.
AudioConnection drumsToLeft(drumMixer, 0, leftMixer, 0);
AudioConnection drumsToRight(drumMixer, 0, rightMixer, 0);
AudioConnection bassToLeft(bassEnvelope, 0, leftMixer, 1);
AudioConnection bassToRight(bassEnvelope, 0, rightMixer, 1);
AudioConnection dryChordToLeft(chordEnvelope, 0, leftMixer, 2);
AudioConnection dryChordToRight(chordEnvelope, 0, rightMixer, 2);
AudioConnection reverbToLeft(chordReverb, 0, leftMixer, 3);
AudioConnection reverbToRight(chordReverb, 1, rightMixer, 3);
AudioConnection leftToOutput(leftMixer, 0, audioOut, 0);
AudioConnection rightToOutput(rightMixer, 0, audioOut, 1);

namespace {

constexpr uint32_t BPM = 125;
constexpr uint32_t STEP_MS = 60000UL / (BPM * 4U); // one sixteenth note
constexpr uint32_t BAR_STEPS = 16;

constexpr int8_t BASS_ROOT_MIDI[4] = {45, 41, 36, 43}; // A2, F2, C2, G2

// A minor 7 - F major 7 - C major 7 - G dominant 7.
constexpr int8_t CHORD_ROOT_MIDI[4]   = {57, 53, 48, 55};
constexpr int8_t CHORD_THIRD_MIDI[4]  = {60, 57, 52, 59};
constexpr int8_t CHORD_FIFTH_MIDI[4]  = {64, 60, 55, 62};
constexpr int8_t CHORD_SEVENTH_MIDI[4]= {67, 64, 59, 65};

uint32_t nextStep = 0;
uint32_t nextStats = 0;
uint32_t stepCounter = 0;

uint32_t clapOffAt = 0;
uint32_t closedHatOffAt = 0;
uint32_t openHatOffAt = 0;
uint32_t bassOffAt = 0;
uint32_t chordOffAt = 0;

bool clapActive = false;
bool closedHatActive = false;
bool openHatActive = false;
bool bassActive = false;
bool chordActive = false;

float midiFrequency(int midiNote)
{
    return 440.0f * powf(2.0f, (static_cast<float>(midiNote) - 69.0f) / 12.0f);
}

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

void updateChord(uint8_t chord)
{
    chordRoot.frequency(midiFrequency(CHORD_ROOT_MIDI[chord]));
    chordThird.frequency(midiFrequency(CHORD_THIRD_MIDI[chord]));
    chordFifth.frequency(midiFrequency(CHORD_FIFTH_MIDI[chord]));
    chordSeventh.frequency(midiFrequency(CHORD_SEVENTH_MIDI[chord]));
}

void playStep(uint32_t now)
{
    const uint8_t stepInBar = static_cast<uint8_t>(stepCounter % BAR_STEPS);
    const uint8_t chord = static_cast<uint8_t>((stepCounter / BAR_STEPS) % 4);

    if (stepInBar == 0) {
        updateChord(chord);
    }

    // Four-on-the-floor kick.
    if ((stepInBar % 4U) == 0) {
        kick.noteOn();
    }

    // Clap/snare on beats 2 and 4.
    if (stepInBar == 4 || stepInBar == 12) {
        triggerEnvelope(clapEnvelope, clapActive, clapOffAt, now, 55);
    }

    // Eighth-note hi-hats. Open hats replace the closed hat on selected
    // offbeats to give the groove some lift.
    if ((stepInBar % 2U) == 0) {
        if (stepInBar == 6 || stepInBar == 14) {
            triggerEnvelope(openHatEnvelope, openHatActive, openHatOffAt, now, 85);
        } else {
            triggerEnvelope(closedHatEnvelope, closedHatActive, closedHatOffAt, now, 24);
        }
    } else if ((stepInBar % 4U) == 3U && random(100) < 18) {
        // Occasional sixteenth-note ghost hat.
        triggerEnvelope(closedHatEnvelope, closedHatActive, closedHatOffAt, now, 18);
    }

    // Syncopated bass: root, fifth and octave. Notes are deliberately short.
    if (stepInBar == 0 || stepInBar == 3 || stepInBar == 6 ||
        stepInBar == 8 || stepInBar == 11 || stepInBar == 14) {
        int bassNote = BASS_ROOT_MIDI[chord];

        if (stepInBar == 3 || stepInBar == 11) {
            bassNote += 7;
        } else if (stepInBar == 6 || stepInBar == 14) {
            bassNote += 12;
        }

        bass.frequency(midiFrequency(bassNote));
        triggerEnvelope(bassEnvelope, bassActive, bassOffAt, now, STEP_MS * 72 / 100);
    }

    // Short chord stabs on the offbeats.
    if (stepInBar == 2 || stepInBar == 6 || stepInBar == 10 || stepInBar == 14) {
        chordEnvelope.noteOn();
        chordActive = true;
        chordOffAt = now + STEP_MS;
    }

    ++stepCounter;
}

void processNoteOffs(uint32_t now)
{
    if (clapActive && timeReached(now, clapOffAt)) {
        clapEnvelope.noteOff();
        clapActive = false;
    }

    if (closedHatActive && timeReached(now, closedHatOffAt)) {
        closedHatEnvelope.noteOff();
        closedHatActive = false;
    }

    if (openHatActive && timeReached(now, openHatOffAt)) {
        openHatEnvelope.noteOff();
        openHatActive = false;
    }

    if (bassActive && timeReached(now, bassOffAt)) {
        bassEnvelope.noteOff();
        bassActive = false;
    }

    if (chordActive && timeReached(now, chordOffAt)) {
        chordEnvelope.noteOff();
        chordActive = false;
    }
}

void printCoreUsage(uint8_t core)
{
    const float usage = AudioStream::processorUsage(core);

    Serial.print("Core ");
    Serial.print(core);
    Serial.print(": ");

    if (usage < 0.0f) {
        Serial.print("n/a");
    } else {
        Serial.print(usage, 1);
        Serial.print("%");
    }
}

void printStreamUsage(const char *name, AudioStream &stream)
{
    Serial.print(name);
    Serial.print(' ');
    Serial.print(stream.AudioProcessorUsage(), 1);
    Serial.print('/');
    Serial.print(stream.AudioProcessorUsageMax(), 1);
    Serial.print('%');
    stream.AudioProcessorUsageMaxReset();
}

void printStatistics()
{
    Serial.println("Node CPU % (current/max):");

    Serial.print("Drums: ");
    printStreamUsage("kick", kick);
    Serial.print(" | ");
    printStreamUsage("noise", percussionNoise);
    Serial.print(" | ");
    printStreamUsage("clap", clapEnvelope);
    Serial.print(" | ");
    printStreamUsage("closedHat", closedHatEnvelope);
    Serial.print(" | ");
    printStreamUsage("openHat", openHatEnvelope);
    Serial.print(" | ");
    printStreamUsage("drumMix", drumMixer);
    Serial.println();

    Serial.print("Bass: ");
    printStreamUsage("osc", bass);
    Serial.print(" | ");
    printStreamUsage("filter", bassFilter);
    Serial.print(" | ");
    printStreamUsage("env", bassEnvelope);
    Serial.println();

    Serial.print("Chords: ");
    printStreamUsage("root", chordRoot);
    Serial.print(" | ");
    printStreamUsage("third", chordThird);
    Serial.print(" | ");
    printStreamUsage("fifth", chordFifth);
    Serial.print(" | ");
    printStreamUsage("seventh", chordSeventh);
    Serial.print(" | ");
    printStreamUsage("mix", chordMixer);
    Serial.print(" | ");
    printStreamUsage("filter", chordFilter);
    Serial.print(" | ");
    printStreamUsage("env", chordEnvelope);
    Serial.print(" | ");
    printStreamUsage("reverb", chordReverb);
    Serial.println();

    Serial.print("Output: ");
    printStreamUsage("leftMix", leftMixer);
    Serial.print(" | ");
    printStreamUsage("rightMix", rightMixer);
    Serial.print(" | ");
    printStreamUsage("i2s", audioOut);
    Serial.println();

    Serial.print("Audio graph CPU ");
    Serial.print(AudioProcessorUsage(), 1);
    Serial.print("% (max ");
    Serial.print(AudioProcessorUsageMax(), 1);
    Serial.print("%), audio blocks ");
    Serial.print(AudioMemoryUsage());
    Serial.print(" (max ");
    Serial.print(AudioMemoryUsageMax());
    Serial.println(")");

    Serial.print("CPU load: ");
    printCoreUsage(0);
    Serial.print(" | ");
    printCoreUsage(1);
    Serial.println();

    AudioProcessorUsageMaxReset();
    AudioMemoryUsageMaxReset();
}

} // namespace

void setup()
{
    Serial.begin(115200);
    delay(200);

    audioOut.setChannelCount(codec.channels());

    Serial.println("Deep House: initializing reverb");
    chordReverb.roomsize(0.38f);
    chordReverb.damping(0.65f);
    if (chordReverb.begin()) {
        Serial.print("Deep House: reverb buffers in ");
        Serial.println(chordReverb.usingPSRAM() ? "PSRAM" : "internal RAM");
    } else {
        Serial.println("WARNING: reverb allocation failed");
    }

    Serial.println("Deep House: initializing audio memory");
    AudioMemory(40);
    Serial.println("Deep House: audio memory ready");

    // Kick: short pitch drop for a round electronic thump.
    kick.frequency(58.0f);
    kick.length(170);
    kick.secondMix(0.0f);
    kick.pitchMod(0.10f);

    // White noise shared by the clap and hi-hat envelopes.
    percussionNoise.amplitude(0.42f);

    clapEnvelope.attack(0.5f);
    clapEnvelope.hold(2.0f);
    clapEnvelope.decay(18.0f);
    clapEnvelope.sustain(0.025f);
    clapEnvelope.release(28.0f);

    closedHatEnvelope.attack(0.3f);
    closedHatEnvelope.hold(0.0f);
    closedHatEnvelope.decay(9.0f);
    closedHatEnvelope.sustain(0.0f);
    closedHatEnvelope.release(10.0f);

    openHatEnvelope.attack(0.5f);
    openHatEnvelope.hold(1.0f);
    openHatEnvelope.decay(48.0f);
    openHatEnvelope.sustain(0.04f);
    openHatEnvelope.release(45.0f);

    drumMixer.gain(0, 0.82f);
    drumMixer.gain(1, 0.44f);
    drumMixer.gain(2, 0.30f);
    drumMixer.gain(3, 0.22f);

    // Smooth sawtooth bass through a low-pass filter.
    bass.begin(0.30f, 55.0f, WAVEFORM_SAWTOOTH);
    bassFilter.setLowpass(0, 280.0f, 0.8f);
    bassEnvelope.attack(2.0f);
    bassEnvelope.hold(0.0f);
    bassEnvelope.decay(45.0f);
    bassEnvelope.sustain(0.12f);
    bassEnvelope.release(32.0f);

    // Warm chord stabs with a filtered sawtooth timbre.
    chordRoot.begin(0.22f, 220.0f, WAVEFORM_SAWTOOTH);
    chordThird.begin(0.22f, 261.63f, WAVEFORM_SAWTOOTH);
    chordFifth.begin(0.22f, 329.63f, WAVEFORM_SAWTOOTH);
    chordSeventh.begin(0.22f, 392.0f, WAVEFORM_SAWTOOTH);

    chordMixer.gain(0, 0.32f);
    chordMixer.gain(1, 0.32f);
    chordMixer.gain(2, 0.32f);
    chordMixer.gain(3, 0.32f);
    chordFilter.setLowpass(0, 1500.0f, 0.75f);

    chordEnvelope.attack(4.0f);
    chordEnvelope.hold(12.0f);
    chordEnvelope.decay(90.0f);
    chordEnvelope.sustain(0.12f);
    chordEnvelope.release(65.0f);

    leftMixer.gain(0, 0.82f);  // drums
    leftMixer.gain(1, 0.78f);  // bass
    leftMixer.gain(2, 0.55f);  // dry chord stabs
    leftMixer.gain(3, 0.40f);  // stereo reverb
    rightMixer.gain(0, 0.82f);
    rightMixer.gain(1, 0.78f);
    rightMixer.gain(2, 0.55f);
    rightMixer.gain(3, 0.40f);

    Serial.println("Deep House: initializing codec");
    if (!codec.enable()) {
        Serial.println("ERROR: audio codec initialization failed");
        while (true) {
            delay(1000);
        }
    }

    codec.volume(0.8f);
    Serial.println("Deep House: codec ready");
    randomSeed(esp_random());

    (void)AudioStream::processorUsage(0);
    (void)AudioStream::processorUsage(1);

    nextStep = millis() + STEP_MS;
    nextStats = millis() + 5000;

    Serial.println("Deep House Groove started");
}

void loop()
{
    const uint32_t now = millis();

    processNoteOffs(now);

    if (timeReached(now, nextStep)) {
        nextStep += STEP_MS;

        // Do not replay accumulated steps after a long pause.
        if (timeReached(now, nextStep)) {
            nextStep = now + STEP_MS;
        }

        playStep(now);
    }

    if (timeReached(now, nextStats)) {
        nextStats = now + 5000;
        printStatistics();
    }

    // Yield to FreeRTOS; the musical clock itself stays nonblocking.
    delay(1);
}
