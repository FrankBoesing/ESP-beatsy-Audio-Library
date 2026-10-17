/*
 * Handpan — Für Elise (Beethoven)
 *
 * Extended handpan arrangement of the public-domain theme, with repeating
 * motifs, answering phrases, a contrasting middle section and coda. The
 * attack is generated procedurally; no recorded audio is bundled.
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

// Equal-tempered pitches for the Für Elise motifs and contrasting middle section.
// Slots: G#3, A3, B3, C4, D4, E4, F4, G4, G#4, A4, B4, C5, D5, D#5, E5, F5.
constexpr float PAD_FREQUENCIES[PAD_COUNT] = {
    207.652f, 220.000f, 246.942f, 261.626f,
    293.665f, 329.628f, 349.228f, 391.995f,
    415.305f, 440.000f, 493.883f, 523.251f,
    587.330f, 622.254f, 659.255f, 698.456f
};

// Lowest-cost phrase preset from the listening comparison (variant C):
// keep the warm, long body modes and let the generated attack supply the strike.
// The optional sustained upper partials are disabled to avoid a second melody.
constexpr float BODY_T60[3] = {3.00f, 3.00f, 4.00f};
constexpr float BODY_RATIOS[3] = {1.000f, 2.011f, 2.984f};
constexpr float BODY_GAIN_BASE[3] = {0.30f, 0.31f, 0.52f};
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
    const float scale = peak > 0 ? (0.65f * 32767.0f) / (float)peak : 0.0f;
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
                                   ATTACK_SAMPLE_COUNT, ATTACK_SAMPLE_RATE, 0.03f)) {
            Serial.print("Failed to configure pad ");
            Serial.println(pad);
            return false;
        }
    }
    return true;
}

// Each velocity is a MIDI-like amplitude (0..127). Offsets permit small timing tweaks.
struct PhraseEvent {
    uint16_t step;
    uint16_t offsetMs;
    uint8_t pad;
    uint8_t velocity;
};

// Twelve 8-second sections form an approximately 96-second arrangement at 120 BPM.
// It revisits the opening, adds answering phrases and a contrasting middle section,
// then returns to the theme and coda; this is an arrangement, not a score transcription.
constexpr uint32_t STEP_MS = 125;
constexpr uint16_t PHRASE_STEPS = 768;
constexpr uint32_t PHRASE_DURATION_MS = STEP_MS * PHRASE_STEPS;

constexpr PhraseEvent PHRASE[] = {
    // Opening theme
    {0,0,14,88}, {1,0,13,79}, {2,0,14,84},
    {3,0,13,74}, {4,0,14,79}, {8,0,10,92},
    {10,0,12,74}, {12,0,11,79}, {16,0,9,92},
    {20,0,3,74}, {22,0,5,79}, {24,0,9,92},
    {26,0,10,74}, {30,0,5,79}, {32,0,8,92},
    {34,0,10,74}, {36,0,11,79}, {40,0,5,92},
    {42,0,14,74}, {43,0,13,79}, {44,0,14,84},
    {45,0,13,74}, {46,0,14,79}, {50,0,10,84},
    {52,0,12,74}, {54,0,11,79}, {58,0,9,84},
    {60,0,3,74}, {62,0,5,79},
    // Opening theme repeat
    {64,0,14,90}, {66,0,13,82}, {68,0,14,87},
    {70,0,13,77}, {72,0,14,92}, {76,0,10,87},
    {78,0,12,77}, {80,0,11,92}, {84,0,9,87},
    {88,0,3,90}, {90,0,5,82}, {92,0,9,87},
    {94,0,10,77}, {98,0,5,82}, {100,0,8,87},
    {102,0,10,77}, {104,0,11,92}, {108,0,5,87},
    {110,0,14,77}, {111,0,13,82}, {112,0,14,88},
    {113,0,13,77}, {114,0,14,82}, {118,0,10,87},
    {120,0,12,90}, {122,0,11,82}, {126,0,9,87},
    // Lower answer
    {128,0,3,92}, {130,0,5,85}, {132,0,9,75},
    {134,0,10,80}, {138,0,5,85}, {140,0,8,75},
    {142,0,10,80}, {144,0,11,88}, {148,0,5,75},
    {150,0,10,80}, {152,0,12,88}, {154,0,11,75},
    {158,0,9,80}, {162,0,3,85}, {164,0,5,75},
    {166,0,9,80}, {168,0,10,88}, {172,0,5,75},
    {174,0,8,80}, {176,0,10,88}, {178,0,11,75},
    {182,0,10,80}, {184,0,9,88}, {188,0,8,75},
    {190,0,9,80},
    // Opening variation
    {192,0,14,88}, {193,0,13,88}, {194,0,14,78},
    {195,0,13,83}, {196,0,14,88}, {200,0,10,92},
    {202,0,12,83}, {204,0,11,88}, {208,0,9,92},
    {212,0,3,83}, {214,0,5,88}, {216,0,9,92},
    {218,0,10,83}, {222,0,5,88}, {224,0,8,92},
    {226,0,10,83}, {228,0,11,88}, {232,0,5,92},
    {234,0,14,83}, {235,0,13,88}, {236,0,14,78},
    {237,0,13,83}, {238,0,14,88}, {242,0,10,78},
    {244,0,12,83}, {246,0,11,88}, {250,0,9,78},
    {252,0,3,83}, {254,0,5,88},
    // Contrasting middle section
    {256,0,10,90}, {258,0,11,76}, {260,0,12,81},
    {262,0,14,86}, {266,0,9,76}, {268,0,15,81},
    {270,0,14,86}, {272,0,12,92}, {276,0,8,81},
    {278,0,14,86}, {280,0,12,92}, {282,0,11,81},
    {286,0,6,86}, {288,0,12,92}, {290,0,11,81},
    {292,0,10,86}, {296,0,5,92}, {298,0,14,81},
    {300,0,12,86}, {302,0,10,76}, {306,0,9,81},
    {310,0,8,86}, {314,0,10,76}, {318,0,11,81},
    // Middle response
    {320,0,11,92}, {322,0,12,79}, {324,0,14,84},
    {326,0,15,74}, {328,0,14,88}, {332,0,12,84},
    {334,0,11,74}, {336,0,10,88}, {340,0,9,84},
    {342,0,15,74}, {344,0,14,88}, {346,0,12,84},
    {350,0,8,74}, {352,0,14,88}, {354,0,12,84},
    {356,0,11,74}, {360,0,6,88}, {362,0,12,84},
    {364,0,11,74}, {366,0,10,79}, {370,0,5,84},
    {372,0,10,74}, {374,0,12,79}, {378,0,11,84},
    {382,0,10,74},
    // Bridge
    {384,0,5,88}, {388,0,9,82}, {392,0,10,92},
    {396,0,11,77}, {400,0,12,90}, {404,0,14,87},
    {406,0,13,77}, {408,0,14,90}, {410,0,13,87},
    {412,0,14,77}, {416,0,10,90}, {418,0,12,87},
    {420,0,11,77}, {424,0,9,90}, {428,0,3,87},
    {430,0,5,77}, {432,0,9,90}, {434,0,10,87},
    {438,0,8,77}, {442,0,9,82}, {446,0,10,87},
    // Main theme returns
    {448,0,14,90}, {449,0,13,85}, {450,0,14,75},
    {451,0,13,80}, {452,0,14,85}, {456,0,10,88},
    {458,0,12,80}, {460,0,11,85}, {464,0,9,88},
    {468,0,3,80}, {470,0,5,85}, {472,0,9,88},
    {474,0,10,80}, {478,0,5,85}, {480,0,8,88},
    {482,0,10,80}, {484,0,11,85}, {488,0,5,88},
    {490,0,14,80}, {491,0,13,85}, {492,0,14,75},
    {493,0,13,80}, {494,0,14,85}, {498,0,10,75},
    {500,0,12,80}, {502,0,11,85}, {506,0,9,75},
    {508,0,3,80}, {510,0,5,85},
    // Answer variation
    {512,0,3,92}, {514,0,5,88}, {516,0,9,78},
    {518,0,10,83}, {522,0,5,88}, {524,0,8,78},
    {526,0,10,83}, {528,0,11,88}, {532,0,5,78},
    {534,0,10,83}, {536,0,12,88}, {538,0,11,78},
    {542,0,9,83}, {546,0,3,88}, {548,0,5,78},
    {550,0,9,83}, {552,0,10,88}, {556,0,5,78},
    {558,0,8,83}, {560,0,10,88}, {562,0,11,78},
    {566,0,10,83}, {568,0,9,88}, {572,0,8,78},
    {574,0,9,83},
    // Middle reprise
    {576,0,10,88}, {578,0,11,76}, {580,0,12,81},
    {582,0,14,86}, {586,0,9,76}, {588,0,15,81},
    {590,0,14,86}, {592,0,12,90}, {596,0,8,81},
    {598,0,14,86}, {600,0,12,90}, {602,0,11,81},
    {606,0,6,86}, {608,0,12,90}, {610,0,11,81},
    {612,0,10,86}, {616,0,5,90}, {618,0,14,81},
    {620,0,12,86}, {622,0,10,76}, {626,0,9,81},
    {630,0,8,86}, {634,0,10,76}, {638,0,11,81},
    // Final theme
    {640,0,14,90}, {642,0,13,79}, {644,0,14,84},
    {646,0,13,74}, {648,0,14,92}, {652,0,10,84},
    {654,0,12,74}, {656,0,11,92}, {660,0,9,84},
    {664,0,3,90}, {666,0,5,79}, {668,0,9,84},
    {670,0,10,74}, {674,0,5,79}, {676,0,8,84},
    {678,0,10,74}, {680,0,11,92}, {684,0,5,84},
    {686,0,14,74}, {687,0,13,79}, {688,0,14,88},
    {689,0,13,74}, {690,0,14,79}, {694,0,10,84},
    {696,0,12,90}, {698,0,11,79}, {702,0,9,84},
    // Coda
    {704,0,14,92}, {706,0,13,82}, {708,0,14,87},
    {710,0,13,77}, {712,0,14,88}, {716,0,10,87},
    {718,0,12,77}, {720,0,11,88}, {724,0,9,87},
    {728,0,3,92}, {732,0,5,82}, {736,0,9,90},
    {740,0,10,77}, {744,0,5,88}, {748,0,9,87},
    {752,0,11,92}, {756,0,10,82}, {760,0,9,90},
    {764,0,13,77}, {766,0,13,82}
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
    Serial.println(" Extended 96-second arrangement; 12 sections");
    Serial.println("======================================");

    // Modal coefficients must be prepared at the selected output sample rate.
    if (!audioOut.setSampleRate((float)OUTPUT_SAMPLE_RATE)) {
        Serial.println("ERROR: Could not configure I2S sample rate");
        while (true) delay(1000);
    }

    // H2 adds a little harmonic roughness without introducing another resonator.
    handpan.setSaturation(0.25f);
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
