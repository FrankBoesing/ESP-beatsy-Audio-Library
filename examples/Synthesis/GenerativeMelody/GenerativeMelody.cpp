/*
 * C64-inspired generative melody.
 *
 * This is an original ESP-beatsy example, not a port of a specific C64 BASIC
 * listing. It uses a diatonic scale, chord tones, small melodic steps and a
 * short repeated motif to make each run different while keeping the result
 * musically coherent.
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

// Lead, bass and a three-note chord pad.
AudioSynthWaveform lead;
AudioEffectEnvelope leadEnvelope;

AudioSynthWaveform bass;
AudioEffectEnvelope bassEnvelope;

AudioSynthWaveform chordRoot;
AudioSynthWaveform chordThird;
AudioSynthWaveform chordFifth;
AudioMixer4 chordMixer;
AudioEffectEnvelope chordEnvelope;

AudioMixer4 mainMixer;

AudioConnection leadToEnvelope(lead, leadEnvelope);
AudioConnection bassToEnvelope(bass, bassEnvelope);
AudioConnection rootToChord(chordRoot, 0, chordMixer, 0);
AudioConnection thirdToChord(chordThird, 0, chordMixer, 1);
AudioConnection fifthToChord(chordFifth, 0, chordMixer, 2);
AudioConnection chordToEnvelope(chordMixer, chordEnvelope);
AudioConnection leadToMain(leadEnvelope, 0, mainMixer, 0);
AudioConnection bassToMain(bassEnvelope, 0, mainMixer, 1);
AudioConnection chordToMain(chordEnvelope, 0, mainMixer, 2);
AudioConnection mainToLeft(mainMixer, 0, audioOut, 0);
AudioConnection mainToRight(mainMixer, 0, audioOut, 1);

namespace {

// C major scale, represented as semitone offsets from C.
constexpr int8_t SCALE_SEMITONES[7] = {0, 2, 4, 5, 7, 9, 11};

// Chord progression: C - G - Am - F.
// Each chord is described using scale-degree indices, starting at C4.
constexpr int8_t CHORD_DEGREES[4][3] = {
    {0, 2, 4}, // C major: C, E, G
    {4, 6, 8}, // G major: G, B, D
    {5, 7, 9}, // A minor: A, C, E
    {3, 5, 7}  // F major: F, A, C
};

constexpr int8_t CHORD_ROOT_MIDI[4]  = {60, 55, 57, 53};
constexpr int8_t CHORD_THIRD_MIDI[4] = {64, 59, 60, 57};
constexpr int8_t CHORD_FIFTH_MIDI[4] = {67, 62, 64, 60};

constexpr uint32_t STEP_MS = 230; // eighth notes, about 130 BPM
constexpr uint32_t MELODY_GATE_PERCENT = 68;

uint32_t nextStep = 0;
uint32_t nextStats = 0;
uint32_t stepCounter = 0;

uint32_t leadNoteOffAt = 0;
uint32_t bassNoteOffAt = 0;
bool leadNoteActive = false;
bool bassNoteActive = false;

int8_t motif[4] = {};
int lastDegree = 4;

int clampDegree(int degree)
{
    if (degree < 0) return 0;
    if (degree > 13) return 13;
    return degree;
}

float midiFrequency(int midiNote)
{
    return 440.0f * powf(2.0f, (static_cast<float>(midiNote) - 69.0f) / 12.0f);
}

int scaleDegreeToMidi(int degree)
{
    degree = clampDegree(degree);
    return 60 + SCALE_SEMITONES[degree % 7] + (degree / 7) * 12;
}

int chooseChordTone(uint8_t chord)
{
    int degree = CHORD_DEGREES[chord][random(3)];

    // Occasionally use the same chord tone an octave higher.
    if (random(4) == 0 && degree <= 6) {
        degree += 7;
    }

    return clampDegree(degree);
}

int chooseNearbyDegree(int previous)
{
    // Repeated steps and small moves are more common than large leaps.
    static constexpr int8_t MOVES[7] = {-2, -1, -1, 0, 1, 1, 2};
    return clampDegree(previous + MOVES[random(7)]);
}

void updateChord(uint8_t chord)
{
    chordRoot.frequency(midiFrequency(CHORD_ROOT_MIDI[chord]));
    chordThird.frequency(midiFrequency(CHORD_THIRD_MIDI[chord]));
    chordFifth.frequency(midiFrequency(CHORD_FIFTH_MIDI[chord]));
    chordEnvelope.noteOn();
}

void playLeadNote(int degree, uint32_t now)
{
    lead.frequency(midiFrequency(scaleDegreeToMidi(degree)));
    leadEnvelope.noteOn();
    leadNoteOffAt = now + (STEP_MS * MELODY_GATE_PERCENT) / 100;
    leadNoteActive = true;
}

void playBassNote(uint8_t chord, uint8_t stepInBar, uint32_t now)
{
    int midiNote = CHORD_ROOT_MIDI[chord] - 12;

    // Lift every third beat by an octave for a little variation.
    if (stepInBar == 4) {
        midiNote += 12;
    }

    bass.frequency(midiFrequency(midiNote));
    bassEnvelope.noteOn();
    bassNoteOffAt = now + (STEP_MS * 170) / 100;
    bassNoteActive = true;
}

void playStep(uint32_t now)
{
    const uint8_t stepInBar = stepCounter % 8;
    const uint8_t chord = (stepCounter / 8) % 4;

    if (stepInBar == 0) {
        updateChord(chord);
    }

    int degree;

    if (stepInBar == 0) {
        degree = chooseChordTone(chord);
        motif[0] = degree;
    } else if (stepInBar < 4) {
        degree = chooseNearbyDegree(lastDegree);
        motif[stepInBar] = degree;
    } else if (stepInBar < 7) {
        // Echo the first half of the bar with a small random variation.
        degree = clampDegree(motif[stepInBar - 4] + random(-1, 2));
    } else {
        // Finish the bar on a chord tone.
        degree = chooseChordTone(chord);
    }

    lastDegree = degree;

    // Leave occasional gaps, except at the start and end of each bar.
    if (stepInBar == 0 || stepInBar == 7 || random(100) >= 18) {
        playLeadNote(degree, now);
    }

    // Bass plays quarter notes.
    if ((stepInBar & 1U) == 0) {
        playBassNote(chord, stepInBar, now);
    }

    ++stepCounter;
}

void printStatistics()
{
    Serial.print("CPU ");
    Serial.print(AudioProcessorUsage(), 1);
    Serial.print("% (max ");
    Serial.print(AudioProcessorUsageMax(), 1);
    Serial.print("%), audio blocks ");
    Serial.print(AudioMemoryUsage());
    Serial.print(" (max ");
    Serial.print(AudioMemoryUsageMax());
    Serial.println(")");

    AudioProcessorUsageMaxReset();
    AudioMemoryUsageMaxReset();
}

} // namespace

void setup()
{
    Serial.begin(115200);
    delay(200);

    AudioMemory(20);

    // Keep a chip-tune-like timbre without making the lead too harsh.
    lead.begin(0.17f, 440.0f, WAVEFORM_SQUARE);
    bass.begin(0.18f, 110.0f, WAVEFORM_TRIANGLE);
    chordRoot.begin(0.11f, 261.63f, WAVEFORM_TRIANGLE);
    chordThird.begin(0.11f, 329.63f, WAVEFORM_TRIANGLE);
    chordFifth.begin(0.11f, 392.00f, WAVEFORM_TRIANGLE);

    leadEnvelope.attack(4.0f);
    leadEnvelope.hold(0.0f);
    leadEnvelope.decay(45.0f);
    leadEnvelope.sustain(0.18f);
    leadEnvelope.release(55.0f);

    bassEnvelope.attack(3.0f);
    bassEnvelope.hold(0.0f);
    bassEnvelope.decay(60.0f);
    bassEnvelope.sustain(0.12f);
    bassEnvelope.release(65.0f);

    chordEnvelope.attack(25.0f);
    chordEnvelope.hold(0.0f);
    chordEnvelope.decay(80.0f);
    chordEnvelope.sustain(0.34f);
    chordEnvelope.release(180.0f);

    chordMixer.gain(0, 0.33f);
    chordMixer.gain(1, 0.33f);
    chordMixer.gain(2, 0.33f);

    mainMixer.gain(0, 0.55f);
    mainMixer.gain(1, 0.42f);
    mainMixer.gain(2, 0.75f);

    if (!codec.enable()) {
        Serial.println("ERROR: audio codec initialization failed");
        while (true) {
            delay(1000);
        }
    }

    audioOut.setChannelCount(codec.channels());
    codec.volume(0.55f);

    randomSeed(esp_random());

    nextStep = millis() + STEP_MS;
    nextStats = millis() + 5000;

    Serial.println("Generative melody started");
}

void loop()
{
    const uint32_t now = millis();

    if (leadNoteActive && static_cast<int32_t>(now - leadNoteOffAt) >= 0) {
        leadEnvelope.noteOff();
        leadNoteActive = false;
    }

    if (bassNoteActive && static_cast<int32_t>(now - bassNoteOffAt) >= 0) {
        bassEnvelope.noteOff();
        bassNoteActive = false;
    }

    if (static_cast<int32_t>(now - nextStep) >= 0) {
        nextStep += STEP_MS;

        // Do not try to replay missed steps after a long pause.
        if (static_cast<int32_t>(now - nextStep) >= 0) {
            nextStep = now + STEP_MS;
        }

        playStep(now);
    }

    if (static_cast<int32_t>(now - nextStats) >= 0) {
        nextStats = now + 5000;
        printStatistics();
    }
}
