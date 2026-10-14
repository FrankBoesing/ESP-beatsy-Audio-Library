/*
 * Neon Noir
 *
 * Original cinematic synth piece with a slow minor-key chord bed, a pulsing
 * sub-bass, a glassy lead, tempo-synchronised echoes and stereo Freeverb.
 *
 * The lead intentionally uses a tiny additive wavetable generated in setup().
 * This exercises AudioSynthWavetable without requiring a large external SF2
 * file; a SoundFont-decoder-generated instrument can replace it later.
 *
 * This is an original composition using broad retro-futuristic soundtrack
 * characteristics. It does not reproduce a melody from Blade Runner.
 */

#include <Audio.h>
#include <math.h>
#include <stdint.h>
#include <esp_system.h>

#include "NewVintageDreamsWavetables.h"

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
// Slow, slightly detuned analog-style pad
// -----------------------------------------------------------------------------

AudioSynthWaveform padRoot;
AudioSynthWaveform padThird;
AudioSynthWaveform padFifth;
AudioSynthWaveform padTop;
AudioMixer4 padMixer;
AudioFilterBiquad padFilter;
AudioEffectEnvelope padEnvelope;

// -----------------------------------------------------------------------------
// Sub-bass pulse
// -----------------------------------------------------------------------------

AudioSynthWaveform bassOsc;
AudioFilterBiquad bassFilter;
AudioEffectEnvelope bassEnvelope;

// -----------------------------------------------------------------------------
// Sample-interpolated wavetable lead and tempo-synchronised echoes
// -----------------------------------------------------------------------------

AudioSynthWavetable glassLead;
AudioSynthWavetable choirLayer;
AudioFilterBiquad leadFilter;
AudioEffectDelay leadDelay(1800);
AudioMixer4 echoMixer;

// -----------------------------------------------------------------------------
// Shared ambience and stereo output
// -----------------------------------------------------------------------------

AudioMixer4 wetMixer;
AudioEffectFreeverbStereo space;
AudioMixer4 leftMixer;
AudioMixer4 rightMixer;

AudioConnection rootToPad(padRoot, 0, padMixer, 0);
AudioConnection thirdToPad(padThird, 0, padMixer, 1);
AudioConnection fifthToPad(padFifth, 0, padMixer, 2);
AudioConnection topToPad(padTop, 0, padMixer, 3);
AudioConnection padToFilter(padMixer, padFilter);
AudioConnection filterToPadEnvelope(padFilter, padEnvelope);

AudioConnection bassToFilter(bassOsc, bassFilter);
AudioConnection filterToBassEnvelope(bassFilter, bassEnvelope);

AudioConnection leadToFilter(glassLead, leadFilter);
AudioConnection leadToDelay(leadFilter, leadDelay);
AudioConnection choirToWet(choirLayer, 0, wetMixer, 3);
AudioConnection echo1(leadDelay, 0, echoMixer, 0);
AudioConnection echo2(leadDelay, 1, echoMixer, 1);
AudioConnection echo3(leadDelay, 2, echoMixer, 2);
AudioConnection echo4(leadDelay, 3, echoMixer, 3);

AudioConnection padToWet(padEnvelope, 0, wetMixer, 0);
AudioConnection echoToWet(echoMixer, 0, wetMixer, 1);
AudioConnection bassToWet(bassEnvelope, 0, wetMixer, 2);
AudioConnection wetToSpace(wetMixer, space);

AudioConnection padToLeft(padEnvelope, 0, leftMixer, 0);
AudioConnection leadToLeft(leadFilter, 0, leftMixer, 1);
AudioConnection bassToLeft(bassEnvelope, 0, leftMixer, 2);
AudioConnection spaceToLeft(space, 0, leftMixer, 3);

AudioConnection padToRight(padEnvelope, 0, rightMixer, 0);
AudioConnection leadToRight(leadFilter, 0, rightMixer, 1);
AudioConnection bassToRight(bassEnvelope, 0, rightMixer, 2);
AudioConnection spaceToRight(space, 1, rightMixer, 3);

AudioConnection leftToOutput(leftMixer, 0, audioOut, 0);
AudioConnection rightToOutput(rightMixer, 0, audioOut, 1);

namespace {

constexpr uint32_t BPM = 76;
constexpr uint32_t STEP_MS = 60000UL / BPM / 2U; // eighth notes
constexpr uint8_t STEPS_PER_BAR = 8;

// Four slow, open voicings: Dm(add9), Bbmaj7, Fmaj7 and Csus2.
constexpr int8_t PAD_NOTES[4][4] = {
    {50, 53, 57, 64}, // D3, F3, A3, E4
    {46, 53, 57, 62}, // Bb2, F3, A3, D4
    {53, 60, 64, 69}, // F3, C4, E4, A4
    {48, 55, 62, 67}  // C3, G3, D4, G4
};
constexpr int8_t BASS_ROOTS[4] = {38, 34, 41, 36}; // D2, Bb1, F2, C2

// Sparse, original lead phrases. Zero is a deliberate rest.
constexpr int8_t LEAD_NOTES[4][STEPS_PER_BAR] = {
    {74,  0, 77, 81,  0, 77, 74, 69}, // D minor
    {74,  0, 77, 81, 86,  0, 81, 77}, // Bb major
    {76,  0, 81, 84,  0, 81, 76, 72}, // F major
    {74,  0, 79, 84,  0, 79, 74, 67}  // Csus2
};

uint32_t nextStep = 0;
uint32_t nextStats = 0;
uint32_t stepCounter = 0;
uint32_t bassOffAt = 0;
uint32_t leadOffAt = 0;
bool bassActive = false;
bool leadActive = false;

float midiFrequency(int midiNote)
{
    return 440.0f * powf(2.0f, (static_cast<float>(midiNote) - 69.0f) / 12.0f);
}

bool timeReached(uint32_t now, uint32_t deadline)
{
    return static_cast<int32_t>(now - deadline) >= 0;
}

void updatePad(uint8_t chord)
{
    // Gentle oscillator offsets create movement without a separate chorus.
    padRoot.frequency(midiFrequency(PAD_NOTES[chord][0]));
    padThird.frequency(midiFrequency(PAD_NOTES[chord][1]) * 0.9970f);
    padFifth.frequency(midiFrequency(PAD_NOTES[chord][2]) * 1.0030f);
    padTop.frequency(midiFrequency(PAD_NOTES[chord][3]) * 1.0015f);

    static const float cutoff[4] = {720.0f, 590.0f, 980.0f, 650.0f};
    padFilter.setLowpass(0, cutoff[chord], 0.72f);
}

void playStep(uint32_t now)
{
    const uint8_t stepInBar = static_cast<uint8_t>(stepCounter % STEPS_PER_BAR);
    const uint8_t chord = static_cast<uint8_t>((stepCounter / STEPS_PER_BAR) % 4U);

    if (stepInBar == 0) {
        updatePad(chord);
        // A sampled choir layer supplies a slow, airy undertone to the chord.
        choirLayer.playNote(PAD_NOTES[chord][3] + 12, 56);
    }

    // A restrained low pulse on the downbeat and halfway through each bar.
    if (stepInBar == 0 || stepInBar == 4) {
        const int bassNote = BASS_ROOTS[chord] + (stepInBar == 4 ? 7 : 0);
        bassOsc.frequency(midiFrequency(bassNote));
        bassEnvelope.noteOn();
        bassActive = true;
        bassOffAt = now + STEP_MS * 140U / 100U;
    }

    const int leadNote = LEAD_NOTES[chord][stepInBar];
    if (leadNote != 0) {
        glassLead.playNote(leadNote, 82);
        leadActive = true;
        leadOffAt = now + STEP_MS * 68U / 100U;
    }

    ++stepCounter;
}

void processNoteOffs(uint32_t now)
{
    if (bassActive && timeReached(now, bassOffAt)) {
        bassEnvelope.noteOff();
        bassActive = false;
    }

    if (leadActive && timeReached(now, leadOffAt)) {
        glassLead.stop();
        leadActive = false;
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
        Serial.print('%');
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

    Serial.print("Pad: ");
    printStreamUsage("root", padRoot);
    Serial.print(" | ");
    printStreamUsage("third", padThird);
    Serial.print(" | ");
    printStreamUsage("fifth", padFifth);
    Serial.print(" | ");
    printStreamUsage("top", padTop);
    Serial.print(" | ");
    printStreamUsage("mix", padMixer);
    Serial.print(" | ");
    printStreamUsage("filter", padFilter);
    Serial.print(" | ");
    printStreamUsage("env", padEnvelope);
    Serial.println();

    Serial.print("Lead: ");
    printStreamUsage("sawTable", glassLead);
    Serial.print(" | ");
    printStreamUsage("lowpass", leadFilter);
    Serial.print(" | ");
    printStreamUsage("chorale", choirLayer);
    Serial.print(" | ");
    printStreamUsage("delay", leadDelay);
    Serial.print(" | ");
    printStreamUsage("echoMix", echoMixer);
    Serial.println();

    Serial.print("Bass: ");
    printStreamUsage("osc", bassOsc);
    Serial.print(" | ");
    printStreamUsage("filter", bassFilter);
    Serial.print(" | ");
    printStreamUsage("env", bassEnvelope);
    Serial.println();

    Serial.print("Space/output: ");
    printStreamUsage("wetMix", wetMixer);
    Serial.print(" | ");
    printStreamUsage("reverb", space);
    Serial.print(" | ");
    printStreamUsage("left", leftMixer);
    Serial.print(" | ");
    printStreamUsage("right", rightMixer);
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

    glassLead.setInstrument(nvd_saw_instrument);
    choirLayer.setInstrument(nvd_chorale_instrument);
    // Tame the strong high harmonics of the sampled saw wave while keeping
    // enough upper-midrange energy for the lead to remain clear.
    leadFilter.setLowpass(0, 1900.0f, 0.80f);

    // Set taps before playback. Each repeat lands one eighth-note later.
    leadDelay.delay(0, STEP_MS);
    leadDelay.delay(1, STEP_MS * 2U);
    leadDelay.delay(2, STEP_MS * 3U);
    leadDelay.delay(3, STEP_MS * 4U);
    echoMixer.gain(0, 0.31f);
    echoMixer.gain(1, 0.24f);
    echoMixer.gain(2, 0.17f);
    echoMixer.gain(3, 0.11f);

    Serial.println("Neon Noir: initializing stereo Freeverb");
    space.roomsize(0.78f);
    space.damping(0.34f);
    if (space.begin()) {
        Serial.print("Neon Noir: reverb buffers in ");
        Serial.println(space.usingPSRAM() ? "PSRAM" : "internal RAM");
    } else {
        Serial.println("WARNING: reverb allocation failed");
    }

    AudioMemory(48);

    padRoot.begin(0.18f, 110.0f, WAVEFORM_BANDLIMIT_SAWTOOTH);
    padThird.begin(0.18f, 164.81f, WAVEFORM_BANDLIMIT_SAWTOOTH);
    padFifth.begin(0.18f, 220.0f, WAVEFORM_BANDLIMIT_SAWTOOTH);
    padTop.begin(0.18f, 329.63f, WAVEFORM_BANDLIMIT_SAWTOOTH);
    padMixer.gain(0, 0.75f);
    padMixer.gain(1, 0.72f);
    padMixer.gain(2, 0.70f);
    padMixer.gain(3, 0.62f);

    padFilter.setLowpass(0, 720.0f, 0.72f);
    padEnvelope.attack(1500.0f);
    padEnvelope.hold(0.0f);
    padEnvelope.decay(800.0f);
    padEnvelope.sustain(0.68f);
    padEnvelope.release(1800.0f);

    bassOsc.begin(0.28f, midiFrequency(BASS_ROOTS[0]), WAVEFORM_TRIANGLE);
    bassFilter.setLowpass(0, 185.0f, 0.82f);
    bassEnvelope.attack(28.0f);
    bassEnvelope.hold(30.0f);
    bassEnvelope.decay(420.0f);
    bassEnvelope.sustain(0.08f);
    bassEnvelope.release(180.0f);

    wetMixer.gain(0, 0.52f); // pad into the reverb
    wetMixer.gain(1, 0.85f); // lead echoes into the reverb
    wetMixer.gain(2, 0.12f); // a little bass ambience
    wetMixer.gain(3, 0.34f); // CC0 SoundFont-derived choir wavetable

    leftMixer.gain(0, 0.34f);  // dry pad
    leftMixer.gain(1, 0.34f);  // dry wavetable lead
    leftMixer.gain(2, 0.40f);  // sub-bass
    leftMixer.gain(3, 0.44f);  // stereo reverb return
    rightMixer.gain(0, 0.34f);
    rightMixer.gain(1, 0.34f);
    rightMixer.gain(2, 0.40f);
    rightMixer.gain(3, 0.44f);

    updatePad(0);
    padEnvelope.noteOn();

    Serial.println("Neon Noir: initializing codec");
    if (!codec.enable()) {
        Serial.println("ERROR: audio codec initialization failed");
        while (true) {
            delay(1000);
        }
    }
    codec.volume(0.76f);

    (void)AudioStream::processorUsage(0);
    (void)AudioStream::processorUsage(1);

    nextStep = millis() + STEP_MS;
    nextStats = millis() + 5000;

    Serial.println("Neon Noir started (76 BPM, original cinematic synth piece)");
}

void loop()
{
    const uint32_t now = millis();

    processNoteOffs(now);

    if (timeReached(now, nextStep)) {
        nextStep += STEP_MS;
        // Do not replay accumulated notes after a long pause in loop().
        if (timeReached(now, nextStep)) {
            nextStep = now + STEP_MS;
        }
        playStep(now);
    }

    if (timeReached(now, nextStats)) {
        nextStats = now + 5000;
        printStatistics();
    }

    delay(1);
}
