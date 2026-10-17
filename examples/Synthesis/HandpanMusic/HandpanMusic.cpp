/*
 * Handpan — Für Elise (Beethoven)
 *
 * A handpan-style arrangement of the public-domain melody, transposed one octave
 * down. The notes are tuned to equal temperament; the strike transient is
 * generated procedurally and no third-party audio recording is bundled.
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

constexpr uint32_t ATTACK_SAMPLE_RATE = 44100;
constexpr uint32_t ATTACK_SAMPLE_COUNT = 1323; // 30 ms at 44.1 kHz
static int16_t attackSample[ATTACK_SAMPLE_COUNT];

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

constexpr uint8_t PAD_COUNT = 16;

// Equal-tempered pitches used by the Für Elise arrangement, one octave below
// the familiar opening register. Slot numbers are documented in the note table:
// G#3, A3, B3, C4, D4, D#4, E4, G4, G#4, A4, B4, C5, D5, D#5, E5, F5.
constexpr float PAD_FREQUENCIES[PAD_COUNT] = {
    207.652f, 220.000f, 246.942f, 261.626f,
    293.665f, 311.127f, 329.628f, 391.995f,
    415.305f, 440.000f, 493.883f, 523.251f,
    587.330f, 622.254f, 659.255f, 698.456f
};

// Lowest-cost phrase preset from the listening comparison (variant C):
// keep the warm, long body modes and let the generated attack supply the strike.
// The optional sustained upper partials are disabled to avoid a second melody.
constexpr float BODY_T60[3] = {3.00f, 3.00f, 4.00f};
constexpr float BODY_RATIOS[3] = {1.000f, 2.011f, 2.984f};
constexpr float BODY_GAIN_BASE[3] = {0.55f, 0.56f, 0.96f};
constexpr float METAL_RATIOS[3] = {5.36f, 5.96f, 10.79f};
constexpr float METAL_T60[3] = {0.25f, 0.12f, 0.08f};
constexpr float METAL_GAINS[3] = {0.0f, 0.0f, 0.0f};
constexpr float METAL_PHASES[3] = {0.72f, 2.20f, 1.48f};

// Three-body-mode levels vary by pad to avoid identical synthetic timbre.
constexpr float BODY_LEVELS[PAD_COUNT][3] = {
    {0.70f,0.10f,0.30f}, {0.66f,0.18f,0.26f},
    {0.62f,0.27f,0.22f}, {0.65f,0.42f,0.16f},
    {0.58f,0.35f,0.28f}, {0.70f,0.55f,0.20f},
    {0.54f,0.44f,0.31f}, {0.50f,0.75f,0.30f},
    {0.58f,0.68f,0.38f}, {0.62f,0.24f,0.18f},
    {0.64f,0.31f,0.24f}, {0.65f,0.23f,0.17f},
    {0.72f,0.32f,0.20f}, {0.66f,0.29f,0.23f},
    {0.61f,0.36f,0.25f}, {0.68f,0.20f,0.15f}
};

constexpr float BODY_PHASES[PAD_COUNT][3] = {
    {0.12f,1.10f,2.05f}, {0.52f,1.75f,0.35f},
    {0.40f,2.05f,1.30f}, {0.76f,0.60f,2.60f},
    {1.12f,1.45f,0.90f}, {0.35f,2.20f,1.85f},
    {0.62f,1.00f,2.40f}, {0.92f,2.55f,1.50f},
    {0.24f,1.65f,2.20f}, {0.82f,0.45f,1.95f},
    {1.42f,2.05f,0.66f}, {0.25f,1.34f,2.72f},
    {1.05f,0.71f,2.36f}, {0.45f,2.50f,1.12f},
    {1.24f,0.20f,2.06f}, {0.73f,1.87f,0.42f}
};

// A compact, deterministic noise burst plus two rapidly decaying sine rings.
// The attack is generated at setup; it contains no recorded audio.
void buildExampleAttack()
{
    uint32_t randomState = 0x4d595df4U;
    int32_t peak = 0;

    for (uint32_t i = 0; i < ATTACK_SAMPLE_COUNT; ++i) {
        const float t = (float)i / (float)ATTACK_SAMPLE_RATE;
        randomState = randomState * 1664525U + 1013904223U;
        const float noise = ((float)(randomState >> 8) * (1.0f / 8388607.5f)) - 1.0f;
        const float noiseEnvelope = expf(-t / 0.009f) * fminf(1.0f, t / 0.0006f);
        const float ring1 = sinf(6.28318530718f * 3650.0f * t) * expf(-t / 0.0038f);
        const float ring2 = sinf(6.28318530718f * 6100.0f * t) * expf(-t / 0.0018f);
        const float value = 0.60f * noise * noiseEnvelope +
                            0.28f * ring1 + 0.12f * ring2;
        int32_t pcm = (int32_t)(value * 32767.0f);
        if (pcm > 32767) pcm = 32767;
        if (pcm < -32768) pcm = -32768;
        attackSample[i] = (int16_t)pcm;
        const int32_t magnitude = pcm < 0 ? -pcm : pcm;
        if (magnitude > peak) peak = magnitude;
    }

    // Normalize in-place to a safe source peak without a large setup() stack array.
    const float scale = peak > 0 ? (0.75f * 32767.0f) / (float)peak : 0.0f;
    for (uint32_t i = 0; i < ATTACK_SAMPLE_COUNT; ++i) {
        attackSample[i] = (int16_t)((float)attackSample[i] * scale);
    }
}

bool configureHandpan()
{
    for (uint8_t pad = 0; pad < PAD_COUNT; ++pad) {
        AudioSynthHandpan::ModeConfig modes[AudioSynthHandpan::MAX_MODES] = {};
        const float f0 = PAD_FREQUENCIES[pad];

        for (uint8_t mode = 0; mode < 3; ++mode) {
            modes[mode].frequencyHz = f0 * BODY_RATIOS[mode];
            modes[mode].gain = BODY_GAIN_BASE[mode] * BODY_LEVELS[pad][mode];
            modes[mode].decaySeconds = BODY_T60[mode];
            modes[mode].phaseRadians = BODY_PHASES[pad][mode];
        }

        for (uint8_t mode = 0; mode < 3; ++mode) {
            const float detune = 1.0f + 0.0015f * ((int)(pad % 3) - 1) * (float)(mode + 1);
            modes[mode + 3].frequencyHz = f0 * METAL_RATIOS[mode] * detune;
            modes[mode + 3].gain = METAL_GAINS[mode];
            modes[mode + 3].decaySeconds = METAL_T60[mode];
            modes[mode + 3].phaseRadians = METAL_PHASES[mode] + 0.17f * (float)pad;
        }

        // Keep the generated transient modest; the modal bank supplies the long ring.
        if (!handpan.configureNote(pad, modes, 6, attackSample,
                                   ATTACK_SAMPLE_COUNT, ATTACK_SAMPLE_RATE, 0.06f)) {
            Serial.print("Failed to configure pad ");
            Serial.println(pad);
            return false;
        }
    }
    return true;
}

// Each velocity is an individual MIDI-like amplitude (0..127). Missing steps are
// rests; offsetMs permits small timing offsets for ornaments if needed.
struct PhraseEvent {
    uint8_t step;
    uint16_t offsetMs;
    uint8_t pad;
    uint8_t velocity;
};

// Sixteenth-note pulse at 120 BPM. The 160-step phrase is a short, recognizable
// handpan arrangement of the Für Elise opening and its answering motifs.
constexpr uint32_t STEP_MS = 125;
constexpr uint8_t PHRASE_STEPS = 160;
constexpr uint32_t PHRASE_DURATION_MS = STEP_MS * PHRASE_STEPS;

constexpr PhraseEvent PHRASE[] = {
    // Opening motif: E4-D#4-E4-D#4-E4, then B3-D4-C4-A3.
    {  0, 0,  6,  92}, {  1, 0,  5,  76}, {  2, 0,  6,  88},
    {  3, 0,  5,  76}, {  4, 0,  6,  92}, {  7, 0,  2,  80},
    {  9, 0,  4,  78}, { 11, 0,  3,  80}, { 14, 0,  1,  90},

    // Answering theme: C4-E4-A4-B4, then E4-G#4-B4-C5.
    { 22, 0,  3,  72}, { 24, 0,  6,  78}, { 28, 0,  9,  84},
    { 32, 0, 10,  87}, { 36, 0,  6,  76}, { 40, 0,  8,  82},
    { 44, 0, 10,  87}, { 48, 0, 11,  90},

    // Repeat the answering figure before rising to the upper E.
    { 54, 0,  6,  74}, { 58, 0,  9,  82}, { 62, 0, 10,  85},
    { 66, 0, 11,  90}, { 72, 0,  6,  86}, { 76, 0, 14,  92},
    { 77, 0, 13,  74}, { 78, 0, 14,  88}, { 79, 0, 13,  74},
    { 80, 0, 14,  94}, { 84, 0, 10,  80}, { 86, 0, 12,  78},
    { 88, 0, 11,  77}, { 92, 0,  9,  87},

    // Lower-register answer followed by a short rising phrase.
    {100, 0,  3,  70}, {102, 0,  6,  74}, {106, 0,  9,  82},
    {110, 0, 10,  85}, {114, 0,  6,  75}, {118, 0, 11,  85},
    {120, 0, 10,  78}, {122, 0,  9,  82},

    // Cadence leading back to the main motif.
    {128, 0, 10,  83}, {130, 0, 11,  86}, {132, 0, 12,  90},
    {136, 0, 14,  94}, {140, 0,  9,  81}, {144, 0, 15,  90},
    {148, 0, 14,  86}, {152, 0, 12,  83}, {156, 0, 14,  91},
    {157, 0, 13,  73}, {158, 0, 14,  90}
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
        const uint32_t dueAt = phraseStartedAt +
                               (uint32_t)event.step * STEP_MS +
                               event.offsetMs;
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
    Serial.println(" Für Elise arrangement one octave down; 20-second loop");
    Serial.println("======================================");

    // Modal coefficients must be prepared at the selected output sample rate.
    if (!audioOut.setSampleRate((float)OUTPUT_SAMPLE_RATE)) {
        Serial.println("ERROR: Could not configure I2S sample rate");
        while (true) delay(1000);
    }

    // H2 adds a little harmonic roughness without introducing another resonator.
    handpan.setSaturation(0.65f);
    buildExampleAttack();
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
    dryWetMixer.gain(0, 0.84f);
    dryWetMixer.gain(1, hallReady ? 0.18f : 0.0f);

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
