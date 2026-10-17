/*
 * Handpan — Für Elise (Beethoven, WoO 59)
 *
 * The note sequence and timing are derived from the public-domain Mutopia
 * Project score (Breitkopf & Härtel, 1888), exported as MIDI. The melody
 * follows the score rather than a newly invented "inspired by" phrase.
 * The 16-pad instrument range requires octave folding at some passages.
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

// Full melody from the Mutopia Project's public-domain Für Elise MIDI.
 // Times are absolute milliseconds from the original score's MIDI timing.
 // The piano's upper staff is used; pitch classes are preserved while notes
 // are folded into the available 16-pad C4–F5 range. Repeated octave pitches
 // are deduplicated when they collapse onto the same pad at the same instant.
struct PhraseEvent {
    uint32_t atMs;
    uint8_t pad;
    uint8_t velocity;
};

constexpr uint32_t PHRASE_DURATION_MS = 130500;

constexpr PhraseEvent PHRASE[] = {
    {     0, 14,  62}, {   208, 13,  62}, {   417, 14,  62},
    {   625, 13,  62}, {   833, 14,  62}, {  1042,  9,  62},
    {  1250, 12,  62}, {  1458, 10,  62}, {  1667,  7,  62},
    {  2292,  0,  62}, {  2500,  2,  62}, {  2708,  7,  62},
    {  2917,  9,  62}, {  3542,  2,  62}, {  3750,  6,  62},
    {  3958,  9,  62}, {  4167, 10,  62}, {  4792,  2,  62},
    {  5000, 14,  62}, {  5208, 13,  62}, {  5417, 14,  62},
    {  5625, 13,  62}, {  5833, 14,  62}, {  6042,  9,  62},
    {  6250, 12,  62}, {  6458, 10,  62}, {  6667,  7,  62},
    {  7292,  0,  62}, {  7500,  2,  62}, {  7708,  7,  62},
    {  7917,  9,  62}, {  8542,  2,  62}, {  8750, 10,  62},
    {  8958,  9,  62}, {  9167,  7,  62}, { 10000,  7,  62},
    { 10625,  9,  62}, { 10833, 10,  62}, { 11042, 12,  62},
    { 11250, 14,  62}, { 11875,  5,  62}, { 12083, 15,  62},
    { 12292, 14,  62}, { 12500, 12,  62}, { 13125,  3,  62},
    { 13333, 14,  62}, { 13542, 12,  62}, { 13750, 10,  62},
    { 14375,  2,  62}, { 14583, 12,  62}, { 14792, 10,  62},
    { 15000,  9,  62}, { 15625,  2,  62}, { 15833, 14,  62},
    { 16458, 14,  62}, { 16667, 14,  62}, { 17292, 13,  62},
    { 17500, 14,  62}, { 18125, 13,  62}, { 18333, 14,  62},
    { 18542, 13,  62}, { 18750, 14,  62}, { 18958, 13,  62},
    { 19167, 14,  62}, { 19375,  9,  62}, { 19583, 12,  62},
    { 19792, 10,  62}, { 20000,  7,  62}, { 20625,  0,  62},
    { 20833,  2,  62}, { 21042,  7,  62}, { 21250,  9,  62},
    { 21875,  2,  62}, { 22083,  6,  62}, { 22292,  9,  62},
    { 22500, 10,  62}, { 23125,  2,  62}, { 23333, 14,  62},
    { 23542, 13,  62}, { 23750, 14,  62}, { 23958, 13,  62},
    { 24167, 14,  62}, { 24375,  9,  62}, { 24583, 12,  62},
    { 24792, 10,  62}, { 25000,  7,  62}, { 25625,  0,  62},
    { 25833,  2,  62}, { 26042,  7,  62}, { 26250,  9,  62},
    { 26875,  2,  62}, { 27083, 10,  62}, { 27292,  9,  62},
    { 27500,  7,  62}, { 28125,  9,  62}, { 28333, 10,  62},
    { 28542, 12,  62}, { 28750,  7,  62}, { 29375,  2,  62},
    { 29375, 10,  62}, { 29583,  3,  62}, { 29583, 10,  62},
    { 29792,  2,  62}, { 29792,  5,  62}, { 29792, 10,  62},
    { 29905,  3,  62}, { 29952,  7,  62}, { 30000, 10,  62},
    { 30833, 15,  62}, { 31146, 14,  62}, { 31250, 14,  62},
    { 31667, 12,  62}, { 32083,  8,  62}, { 32396,  7,  62},
    { 32500,  7,  62}, { 32708,  5,  62}, { 32917, 15,  62},
    { 33125, 14,  62}, { 33333, 12,  62}, { 33542, 10,  62},
    { 33750,  8,  62}, { 34167,  7,  62}, { 34559,  8,  62},
    { 34583,  7,  62}, { 34687,  5,  62}, { 34792,  7,  62},
    { 34896,  8,  62}, { 35000, 10,  62}, { 35833, 12,  62},
    { 36042, 13,  62}, { 36250, 14,  62}, { 36875, 14,  62},
    { 37083, 15,  62}, { 37292,  7,  62}, { 37500, 10,  62},
    { 38333, 12,  62}, { 38646,  9,  62}, { 38750, 10,  62},
    { 38854,  5,  62}, { 38958,  5,  62}, { 39062,  5,  62},
    { 39167,  7,  62}, { 39271,  5,  62}, { 39375,  9,  62},
    { 39479,  5,  62}, { 39583, 10,  62}, { 39687,  5,  62},
    { 39792, 12,  62}, { 39896,  5,  62}, { 40000, 14,  62},
    { 40104,  5,  62}, { 40208, 10,  62}, { 40312,  9,  62},
    { 40417,  7,  62}, { 40521,  5,  62}, { 40625, 15,  62},
    { 40729, 14,  62}, { 40833, 12,  62}, { 40937,  5,  62},
    { 41042, 15,  62}, { 41146, 12,  62}, { 41250, 10,  62},
    { 41354,  5,  62}, { 41458,  5,  62}, { 41562,  5,  62},
    { 41667,  7,  62}, { 41771,  5,  62}, { 41875,  9,  62},
    { 41979,  5,  62}, { 42083, 10,  62}, { 42187,  5,  62},
    { 42292, 12,  62}, { 42396,  5,  62}, { 42500, 14,  62},
    { 42604,  5,  62}, { 42708, 10,  62}, { 42812,  9,  62},
    { 42917,  7,  62}, { 43021,  5,  62}, { 43125, 15,  62},
    { 43229, 14,  62}, { 43333, 12,  62}, { 43437,  5,  62},
    { 43542, 15,  62}, { 43646, 12,  62}, { 43750, 14,  62},
    { 43854, 15,  62}, { 43958, 14,  62}, { 44062, 13,  62},
    { 44167, 14,  62}, { 44271,  9,  62}, { 44375, 14,  62},
    { 44479, 13,  62}, { 44583, 14,  62}, { 44687,  9,  62},
    { 44792, 14,  62}, { 44896, 13,  62}, { 45000, 14,  62},
    { 45625,  9,  62}, { 45833, 14,  62}, { 46042, 13,  62},
    { 46250, 14,  62}, { 46875,  9,  62}, { 47083, 14,  62},
    { 47292, 13,  62}, { 47500, 14,  62}, { 47708, 13,  62},
    { 47917, 14,  62}, { 48125, 13,  62}, { 48333, 14,  62},
    { 48542, 13,  62}, { 48750, 14,  62}, { 48958, 13,  62},
    { 49167, 14,  62}, { 49375,  9,  62}, { 49583, 12,  62},
    { 49792, 10,  62}, { 50000,  7,  62}, { 50625,  0,  62},
    { 50833,  2,  62}, { 51042,  7,  62}, { 51250,  9,  62},
    { 51875,  2,  62}, { 52083,  6,  62}, { 52292,  9,  62},
    { 52500, 10,  62}, { 53125,  2,  62}, { 53333, 14,  62},
    { 53542, 13,  62}, { 53750, 14,  62}, { 53958, 13,  62},
    { 54167, 14,  62}, { 54375,  9,  62}, { 54583, 12,  62},
    { 54792, 10,  62}, { 55000,  7,  62}, { 55625,  0,  62},
    { 55833,  2,  62}, { 56042,  7,  62}, { 56250,  9,  62},
    { 56875,  2,  62}, { 57083, 10,  62}, { 57292,  9,  62},
    { 57500,  7,  62}, { 58125,  9,  62}, { 58333, 10,  62},
    { 58542, 12,  62}, { 58750, 14,  62}, { 59375,  5,  62},
    { 59583, 15,  62}, { 59792, 14,  62}, { 60000, 12,  62},
    { 60625,  3,  62}, { 60833, 14,  62}, { 61042, 12,  62},
    { 61250, 10,  62}, { 61875,  2,  62}, { 62083, 12,  62},
    { 62292, 10,  62}, { 62500,  9,  62}, { 63125,  2,  62},
    { 63333, 14,  62}, { 63958, 14,  62}, { 64167, 14,  62},
    { 64792, 13,  62}, { 65000, 14,  62}, { 65625, 13,  62},
    { 65833, 14,  62}, { 66042, 13,  62}, { 66250, 14,  62},
    { 66458, 13,  62}, { 66667, 14,  62}, { 66875,  9,  62},
    { 67083, 12,  62}, { 67292, 10,  62}, { 67500,  7,  62},
    { 68125,  0,  62}, { 68333,  2,  62}, { 68542,  7,  62},
    { 68750,  9,  62}, { 69375,  2,  62}, { 69583,  6,  62},
    { 69792,  9,  62}, { 70000, 10,  62}, { 70625,  2,  62},
    { 70833, 14,  62}, { 71042, 13,  62}, { 71250, 14,  62},
    { 71458, 13,  62}, { 71667, 14,  62}, { 71875,  9,  62},
    { 72083, 12,  62}, { 72292, 10,  62}, { 72500,  7,  62},
    { 73125,  0,  62}, { 73333,  2,  62}, { 73542,  7,  62},
    { 73750,  9,  62}, { 74375,  2,  62}, { 74583, 10,  62},
    { 74792,  9,  62}, { 75000,  7,  62}, { 76250,  2,  62},
    { 76250,  5,  62}, { 76250,  8,  62}, { 76250, 11,  62},
    { 77500,  3,  62}, { 77500,  7,  62}, { 77500, 12,  62},
    { 78333, 11,  62}, { 78333, 14,  62}, { 78542, 12,  62},
    { 78542, 15,  62}, { 78750,  6,  62}, { 78750, 12,  62},
    { 78750, 15,  62}, { 79583,  6,  62}, { 79583, 12,  62},
    { 79583, 15,  62}, { 80000,  7,  62}, { 80000, 10,  62},
    { 80000, 14,  62}, { 81250,  3,  62}, { 81250, 12,  62},
    { 82083,  2,  62}, { 82083, 10,  62}, { 82292,  9,  62},
    { 82292, 12,  62}, { 82500,  0,  62}, { 82500,  4,  62},
    { 82500,  7,  62}, { 83333,  0,  62}, { 83333,  7,  62},
    { 83750,  0,  62}, { 83750,  7,  62}, { 84167,  2,  62},
    { 84167, 10,  62}, { 84583,  9,  62}, { 84583, 12,  62},
    { 85000,  0,  62}, { 85000,  7,  62}, { 86250,  2,  62},
    { 86250,  5,  62}, { 86250,  8,  62}, { 86250, 11,  62},
    { 87500,  3,  62}, { 87500,  7,  62}, { 87500, 12,  62},
    { 88333, 11,  62}, { 88333, 14,  62}, { 88542, 12,  62},
    { 88542, 15,  62}, { 88750, 12,  62}, { 88750, 15,  62},
    { 89583, 12,  62}, { 89583, 15,  62}, { 90000, 12,  62},
    { 90000, 15,  62}, { 91250,  5,  62}, { 91250, 13,  62},
    { 92083,  3,  62}, { 92083, 12,  62}, { 92292,  1,  62},
    { 92292, 10,  62}, { 92500,  3,  62}, { 92500,  8,  62},
    { 92500, 12,  62}, { 93333,  3,  62}, { 93333,  7,  62},
    { 93333, 12,  62}, { 93750,  3,  62}, { 93750,  6,  62},
    { 93750, 12,  62}, { 94583,  3,  62}, { 94583,  6,  62},
    { 94583, 12,  62}, { 95000,  0,  62}, { 95000,  2,  62},
    { 95000,  7,  62}, { 96250,  2,  62}, { 96250,  9,  62},
    { 97500,  7,  62}, { 97639,  0,  62}, { 97778,  2,  62},
    { 97917,  7,  62}, { 98056, 10,  62}, { 98194, 14,  62},
    { 98333, 12,  62}, { 98472, 10,  62}, { 98611,  9,  62},
    { 98750,  7,  62}, { 98889, 10,  62}, { 99028, 14,  62},
    { 99167,  7,  62}, { 99306, 10,  62}, { 99444, 14,  62},
    { 99583, 12,  62}, { 99722, 10,  62}, { 99861,  9,  62},
    {100000,  7,  62}, {100139, 10,  62}, {100278, 14,  62},
    {100417,  7,  62}, {100556, 10,  62}, {100694, 14,  62},
    {100833, 12,  62}, {100972, 10,  62}, {101111,  9,  62},
    {101250,  8,  62}, {101389,  7,  62}, {101528,  6,  62},
    {101667,  5,  62}, {101806,  4,  62}, {101944, 15,  62},
    {102083, 14,  62}, {102222, 13,  62}, {102361, 12,  62},
    {102500, 11,  62}, {102639, 10,  62}, {102778,  9,  62},
    {102917,  8,  62}, {103056,  7,  62}, {103194,  6,  62},
    {103333,  5,  62}, {103472,  4,  62}, {103611, 15,  62},
    {103750, 14,  62}, {103958, 13,  62}, {104167, 14,  62},
    {104375,  9,  62}, {104583, 12,  62}, {104792, 10,  62},
    {105000,  7,  62}, {105625,  0,  62}, {105833,  2,  62},
    {106042,  7,  62}, {106250,  9,  62}, {106875,  2,  62},
    {107083,  6,  62}, {107292,  9,  62}, {107500, 10,  62},
    {108125,  2,  62}, {108333, 14,  62}, {108542, 13,  62},
    {108750, 14,  62}, {108958, 13,  62}, {109167, 14,  62},
    {109375,  9,  62}, {109583, 12,  62}, {109792, 10,  62},
    {110000,  7,  62}, {110625,  0,  62}, {110833,  2,  62},
    {111042,  7,  62}, {111250,  9,  62}, {111875,  2,  62},
    {112083, 10,  62}, {112292,  9,  62}, {112500,  7,  62},
    {113125,  9,  62}, {113333, 10,  62}, {113542, 12,  62},
    {113750, 14,  62}, {114375,  5,  62}, {114583, 15,  62},
    {114792, 14,  62}, {115000, 12,  62}, {115625,  3,  62},
    {115833, 14,  62}, {116042, 12,  62}, {116250, 10,  62},
    {116875,  2,  62}, {117083, 12,  62}, {117292, 10,  62},
    {117500,  9,  62}, {118125,  2,  62}, {118333, 14,  62},
    {118958, 14,  62}, {119167, 14,  62}, {119792, 13,  62},
    {120000, 14,  62}, {120625, 13,  62}, {120833, 14,  62},
    {121042, 13,  62}, {121250, 14,  62}, {121458, 13,  62},
    {121667, 14,  62}, {121875,  9,  62}, {122083, 12,  62},
    {122292, 10,  62}, {122500,  7,  62}, {123125,  0,  62},
    {123333,  2,  62}, {123542,  7,  62}, {123750,  9,  62},
    {124375,  2,  62}, {124583,  6,  62}, {124792,  9,  62},
    {125000, 10,  62}, {125625,  2,  62}, {125833, 14,  62},
    {126042, 13,  62}, {126250, 14,  62}, {126458, 13,  62},
    {126667, 14,  62}, {126875,  9,  62}, {127083, 12,  62},
    {127292, 10,  62}, {127500,  7,  62}, {128125,  0,  62},
    {128333,  2,  62}, {128542,  7,  62}, {128750,  9,  62},
    {129375,  2,  62}, {129583, 10,  62}, {129792,  9,  62},
    {130000,  7,  62}
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
    Serial.printf(" Original-score melody; loop length %.1f seconds; %u events\n",
                  PHRASE_DURATION_MS / 1000.0f, (unsigned)PHRASE_EVENT_COUNT);
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
