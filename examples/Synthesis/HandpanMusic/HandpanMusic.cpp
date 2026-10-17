/*
 * Handpan — Für Elise, opening excerpt (Beethoven, WoO 59)
 *
 * Uses the first 30 seconds of the upper-staff melody from the current,
 * public-domain Mutopia Project MIDI derived from the Breitkopf & Härtel
 * (1888) edition. At 25.833 s, the isolated E6 is lowered to E5 for this
 * handpan arrangement; the original note timings are retained.
 * Source: https://www.mutopiaproject.org/cgibin/piece-info.cgi?id=931
 */

#include <Audio.h>
#include <math.h>

// Standalone musical demonstration. Set HANDPAN_MUSIC_SAMPLE_RATE to 44100
// for a 44.1-kHz run; 48 kHz is the default.
// The output clock is set before configuring the resonators.
#ifndef HANDPAN_MUSIC_SAMPLE_RATE
#define HANDPAN_MUSIC_SAMPLE_RATE 48000
#endif
#if HANDPAN_MUSIC_SAMPLE_RATE != 44100 && HANDPAN_MUSIC_SAMPLE_RATE != 48000
#error "HANDPAN_MUSIC_SAMPLE_RATE must be 44100 or 48000"
#endif
constexpr uint32_t OUTPUT_SAMPLE_RATE = HANDPAN_MUSIC_SAMPLE_RATE;

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

AudioSynthHandpan handpan;
AudioEffectFreeverb hall;
AudioMixer4 dryWetMixer;
AudioConnection handpanToHall(handpan, hall);
AudioConnection handpanToDryMixer(handpan, 0, dryWetMixer, 0);
AudioConnection hallToWetMixer(hall, 0, dryWetMixer, 1);
AudioConnection mixToLeft(dryWetMixer, 0, audioOut, 0);
AudioConnection mixToRight(dryWetMixer, 0, audioOut, 1);

namespace {

// Pitches used in the first 30 seconds of the score-derived melody.
// The isolated E6 at 25.833 s is lowered by one octave to E5 for this arrangement.
// MIDI pitches after this adjustment: 60, 64, 65, 67, 68, 69, 71, 72, 74, 75, 76, 77.
constexpr uint8_t PAD_COUNT = 12;
// Pad-index to MIDI pitch: 60, 64, 65, 67, 68, 69, 71, 72, 74, 75, 76, 77.
constexpr float PAD_FREQUENCIES[PAD_COUNT] = {
    261.626f, 329.628f, 349.228f, 391.995f,
    415.305f, 440.000f, 493.883f, 523.251f,
    587.330f, 622.254f, 659.255f, 698.456f
};

// Keep the warm body modes and add a soft, damped finger-contact transient.
// The upper partials remain disabled to limit CPU and avoid a second melody.
constexpr float BODY_T60[3] = {3.00f, 3.00f, 4.00f};
constexpr float BODY_RATIOS[3] = {1.000f, 2.011f, 2.984f};
constexpr float BODY_GAIN_BASE[3] = {0.30f, 0.31f, 0.52f};
constexpr float METAL_RATIOS[3] = {5.36f, 5.96f, 10.79f};
constexpr float METAL_T60[3] = {0.25f, 0.12f, 0.08f};
constexpr float METAL_GAINS[3] = {0.0f, 0.0f, 0.0f};
constexpr float METAL_PHASES[3] = {0.72f, 2.20f, 1.48f};

// Three-body-mode levels vary by pad to avoid identical synthetic timbre.
// Keep resonator phase at zero in this listening test: each new strike then
// starts at a zero crossing instead of injecting an abrupt nonzero first sample.
constexpr float BODY_LEVELS[PAD_COUNT][3] = {
    {0.70f,0.10f,0.30f},
    {0.66f,0.18f,0.26f},
    {0.62f,0.27f,0.22f},
    {0.65f,0.42f,0.16f},
    {0.58f,0.35f,0.28f},
    {0.70f,0.55f,0.20f},
    {0.54f,0.44f,0.31f},
    {0.50f,0.75f,0.30f},
    {0.58f,0.68f,0.38f},
    {0.62f,0.24f,0.18f},
    {0.64f,0.31f,0.24f},
    {0.65f,0.23f,0.17f}
};

bool configureHandpan()
{
    for (uint8_t pad = 0; pad < PAD_COUNT; ++pad) {
        AudioSynthHandpan::ModeConfig modes[AudioSynthHandpan::MAX_MODES] = {};
        const float f0 = PAD_FREQUENCIES[pad];

        for (uint8_t mode = 0; mode < 3; ++mode) {
            modes[mode].frequencyHz = f0 * BODY_RATIOS[mode];
            modes[mode].gain = BODY_GAIN_BASE[mode] * BODY_LEVELS[pad][mode];
            modes[mode].decaySeconds = BODY_T60[mode];
            modes[mode].phaseRadians = 0.0f;
        }

        for (uint8_t mode = 0; mode < 3; ++mode) {
            const float detune = 1.0f + 0.0015f * ((int)(pad % 3) - 1) * (float)(mode + 1);
            modes[mode + 3].frequencyHz = f0 * METAL_RATIOS[mode] * detune;
            modes[mode + 3].gain = METAL_GAINS[mode];
            modes[mode + 3].decaySeconds = METAL_T60[mode];
            modes[mode + 3].phaseRadians = METAL_PHASES[mode] + 0.17f * (float)pad;
        }

        // The synth supplies its built-in soft finger-contact transient.
        // No attack buffer or waveform-generation code is needed in this sketch.
        if (!handpan.configureNote(pad, modes, 6)) {
            Serial.print("Failed to configure pad ");
            Serial.println(pad);
            return false;
        }
    }
    return true;
}

// Score-derived upper-staff excerpt, truncated at 30,000 ms.
// Note onset times are preserved; the isolated E6 is lowered to E5.
struct PhraseEvent {
    uint32_t atMs;
    uint8_t pad;
    uint8_t velocity;
};

constexpr uint32_t PHRASE_DURATION_MS = 30000;

constexpr PhraseEvent PHRASE[] = {
    {    0, 10,  62}, {  208,  9,  62}, {  417, 10,  62},
    {  625,  9,  62}, {  833, 10,  62}, { 1042,  6,  62},
    { 1250,  8,  62}, { 1458,  7,  62}, { 1667,  5,  62},
    { 2292,  0,  62}, { 2500,  1,  62}, { 2708,  5,  62},
    { 2917,  6,  62}, { 3542,  1,  62}, { 3750,  4,  62},
    { 3958,  6,  62}, { 4167,  7,  62}, { 4792,  1,  62},
    { 5000, 10,  62}, { 5208,  9,  62}, { 5417, 10,  62},
    { 5625,  9,  62}, { 5833, 10,  62}, { 6042,  6,  62},
    { 6250,  8,  62}, { 6458,  7,  62}, { 6667,  5,  62},
    { 7292,  0,  62}, { 7500,  1,  62}, { 7708,  5,  62},
    { 7917,  6,  62}, { 8542,  1,  62}, { 8750,  7,  62},
    { 8958,  6,  62}, { 9167,  5,  62}, {10000, 10,  62},
    {10208,  9,  62}, {10417, 10,  62}, {10625,  9,  62},
    {10833, 10,  62}, {11042,  6,  62}, {11250,  8,  62},
    {11458,  7,  62}, {11667,  5,  62}, {12292,  0,  62},
    {12500,  1,  62}, {12708,  5,  62}, {12917,  6,  62},
    {13542,  1,  62}, {13750,  4,  62}, {13958,  6,  62},
    {14167,  7,  62}, {14792,  1,  62}, {15000, 10,  62},
    {15208,  9,  62}, {15417, 10,  62}, {15625,  9,  62},
    {15833, 10,  62}, {16042,  6,  62}, {16250,  8,  62},
    {16458,  7,  62}, {16667,  5,  62}, {17292,  0,  62},
    {17500,  1,  62}, {17708,  5,  62}, {17917,  6,  62},
    {18542,  1,  62}, {18750,  7,  62}, {18958,  6,  62},
    {19167,  5,  62}, {19792,  6,  62}, {20000,  7,  62},
    {20208,  8,  62}, {20417, 10,  62}, {21042,  3,  62},
    {21250, 11,  62}, {21458, 10,  62}, {21667,  8,  62},
    {22292,  2,  62}, {22500, 10,  62}, {22708,  8,  62},
    {22917,  7,  62}, {23542,  1,  62}, {23750,  8,  62},
    {23958,  7,  62}, {24167,  6,  62}, {24792,  1,  62},
    {25000, 10,  62}, {25208,  1,  62}, {25417, 10,  62},
    {25625, 10,  62}, {25833, 10,  62}, {26042,  9,  62},
    {26250, 10,  62}, {26458,  9,  62}, {26667, 10,  62},
    {26875,  9,  62}, {27083, 10,  62}, {27292,  9,  62},
    {27500, 10,  62}, {27708,  9,  62}, {27917, 10,  62},
    {28125,  9,  62}, {28333, 10,  62}, {28542,  6,  62},
    {28750,  8,  62}, {28958,  7,  62}, {29167,  5,  62},
    {29792,  0,  62}
};

constexpr size_t PHRASE_EVENT_COUNT = sizeof(PHRASE) / sizeof(PHRASE[0]);
uint32_t phraseStartedAt = 0;
size_t nextEventIndex = 0;
uint32_t droppedEvents = 0;

void playDueEvents(uint32_t now)
{
    // Keep the event schedule anchored to phrase time rather than accumulating
    // loop jitter. If the loop is late, submit all due events in order.
    while ((int32_t)(now - (phraseStartedAt + PHRASE_DURATION_MS)) >= 0) {
        if (droppedEvents != 0) {
            Serial.printf("Warning: %lu note events were not queued in the previous phrase\n",
                          (unsigned long)droppedEvents);
            droppedEvents = 0;
        }
        phraseStartedAt += PHRASE_DURATION_MS;
        nextEventIndex = 0;
    }

    while (nextEventIndex < PHRASE_EVENT_COUNT) {
        const PhraseEvent &event = PHRASE[nextEventIndex];
        const uint32_t dueAt = phraseStartedAt + event.atMs;
        if ((int32_t)(now - dueAt) < 0) {
            break;
        }

        if (!handpan.noteOn(event.pad, event.velocity)) {
            ++droppedEvents;
        }
        ++nextEventIndex;
    }
}

} // namespace

void setup()
{
    Serial.begin(115200);
    delay(250);

    Serial.println();
    Serial.println("======================================");
    Serial.println(" Handpan — Für Elise");
    Serial.printf(" Output sample rate: %lu Hz\n", (unsigned long)OUTPUT_SAMPLE_RATE);
    Serial.printf(" Opening excerpt; %.1f seconds; %u note events; soft finger attack\n",
                  PHRASE_DURATION_MS / 1000.0f, (unsigned)PHRASE_EVENT_COUNT);
    Serial.println("======================================");

    // Modal coefficients must be prepared at the selected output sample rate.
    if (!audioOut.setSampleRate((float)OUTPUT_SAMPLE_RATE)) {
        Serial.println("ERROR: Could not configure I2S sample rate");
        while (true) delay(1000);
    }

    // H2 adds a little harmonic roughness without introducing another resonator.
    handpan.setSaturation(0.25f);
    if (!configureHandpan()) {
        Serial.println("ERROR: Could not configure the handpan pad bank");
        while (true) delay(1000);
    }

    hall.roomsize(0.68f);
    hall.damping(0.46f);
    const bool hallReady = hall.begin();
    if (!hallReady) {
        Serial.println("WARNING: Freeverb allocation failed; continuing without wet return");
    }
    dryWetMixer.gain(0, 0.72f);
    dryWetMixer.gain(1, hallReady ? 0.14f : 0.0f);

    audioOut.setChannelCount(codec.channels());
    AudioMemory(32);

    bool codecReady = false;
#if defined(AUDIO_CODEC_ES8311)
    codecReady = codec.enable(OUTPUT_SAMPLE_RATE);
#else
    codecReady = codec.enable();
#endif
    if (!codecReady) {
        Serial.println("ERROR: Audio codec initialization failed");
        while (true) delay(1000);
    }
    codec.volume(0.2f);

    phraseStartedAt = millis();
    nextEventIndex = 0;
}

void loop()
{
    playDueEvents(millis());
}
