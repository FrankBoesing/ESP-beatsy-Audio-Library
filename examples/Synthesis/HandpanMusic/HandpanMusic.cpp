/*
 * Handpan — Moonlit Current
 *
 * A looping eight-bar handpan-inspired phrase with dynamic accents, quiet grace
 * notes, resonator tails and a restrained hall return. The short attack transient
 * is generated procedurally; no third-party recording is bundled.
 * Pad frequencies and modal parameters are starting points, not measured tuning.
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

// The 16 pads cover a broad handpan-inspired register. Frequencies are adjustable
// starter values, not a measured tuning for a specific commercial instrument.
constexpr float PAD_FREQUENCIES[PAD_COUNT] = {
    139.6f, 156.8f, 185.0f, 209.4f,
    233.1f, 246.5f, 277.1f, 293.7f,
    312.6f, 330.7f, 349.2f, 372.3f,
    415.7f, 440.0f, 493.9f, 523.3f
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
                                   ATTACK_SAMPLE_COUNT, ATTACK_SAMPLE_RATE, 0.09f)) {
            Serial.print("Failed to configure pad ");
            Serial.println(pad);
            return false;
        }
    }
    return true;
}

// One eighth-note step at about 96 BPM. The eight-bar phrase lasts about 20 s.
// Each velocity is an individual MIDI-like amplitude (0..127). Missing steps are
// rests; offsetMs adds a quiet grace note between the main beats.
struct PhraseEvent {
    uint8_t step;
    uint16_t offsetMs;
    uint8_t pad;
    uint8_t velocity;
};

constexpr uint32_t STEP_MS = 313;
constexpr uint8_t PHRASE_STEPS = 64;
constexpr uint32_t PHRASE_DURATION_MS = STEP_MS * PHRASE_STEPS;

constexpr PhraseEvent PHRASE[] = {
    // Bar 1 — introduce the motif gently, then answer it with a stronger note.
    { 0,   0,  0, 100}, { 1,   0,  4,  64}, { 2,   0,  2,  82},
    { 3,   0,  5,  48}, { 4,   0,  1,  88}, { 5,   0,  6,  66},
    { 6,   0,  4,  54}, { 7,   0,  7,  96}, { 7, 145,  3,  40},

    // Bar 2 — a descending answer with a quiet upper grace note.
    { 8,   0,  3,  82}, { 9,   0,  2,  57}, {10,   0,  6,  76},
    {12,   0,  5,  92}, {13,   0,  1,  62}, {14,   0,  7,  78},
    {15,   0,  2, 101}, {15, 155,  9,  37},

    // Bar 3 — revisit the opening figure with softer inner notes.
    {16,   0,  0,  96}, {17,   0,  4,  62}, {18,   0,  2,  79},
    {18, 145, 11,  41}, {19,   0,  6,  58}, {20,   0,  5,  87},
    {22,   0,  7,  91}, {23,   0,  3,  49},

    // Bar 4 — brighter cadence, with a soft ornament after the last note.
    {24,   0,  2,  84}, {25,   0,  6,  54}, {26,   0,  1,  77},
    {27,   0,  5,  47}, {28,   0,  0,  99}, {29,   0,  4,  64},
    {30,   0,  7,  84}, {31,   0,  2, 104}, {31, 145, 13,  35},

    // Bar 5 — return to the motif with a little more energy.
    {32,   0,  0, 103}, {33,   0,  5,  70}, {34,   0,  2,  78},
    {35,   0,  6,  52}, {36,   0,  1,  87}, {37,   0,  7,  62},
    {38,   0,  4,  57}, {39,   0,  9,  84},

    // Bar 6 — sparse response; rests let the longer body modes breathe.
    {40,   0,  3,  81}, {41,   0,  2,  54}, {42,   0,  6,  74},
    {43,   0,  0,  46}, {44,   0,  5,  92}, {45,   0,  1,  61},
    {46,   0,  7,  80}, {47,   0,  2, 105}, {47, 150, 14,  33},

    // Bar 7 — broaden the register with a quiet upper answering note.
    {48,   0,  0,  98}, {49,   0,  4,  66}, {50,   0,  2,  83},
    {50, 150, 12,  40}, {51,   0,  5,  51}, {52,   0,  1,  86},
    {53,   0,  6,  67}, {54,   0,  4,  55}, {55,   0,  7,  94},

    // Bar 8 — resolve the phrase and leave space before it loops.
    {56,   0,  3,  83}, {57,   0,  2,  58}, {58,   0,  6,  76},
    {60,   0,  5,  94}, {61,   0,  1,  64}, {62,   0,  7,  80},
    {63,   0,  2, 102}, {63, 145, 15,  34}
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
    Serial.println(" Handpan — Moonlit Current");
    Serial.printf(" Output sample rate: %lu Hz\n", (unsigned long)OUTPUT_SAMPLE_RATE);
    Serial.println(" Eight-bar phrase; variable velocity; looping with resonator tails");
    Serial.println("======================================");

    // Modal coefficients must be prepared at the selected output sample rate.
    if (!audioOut.setSampleRate((float)OUTPUT_SAMPLE_RATE)) {
        Serial.println("ERROR: Could not configure I2S sample rate");
        while (true) delay(1000);
    }

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
