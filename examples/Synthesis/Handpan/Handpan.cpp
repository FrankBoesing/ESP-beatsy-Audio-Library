#include <Audio.h>
#include <math.h>

// This load test generates its attack transient at startup. No recorded
// handpan audio is required. Applications may still pass their own PCM16 attack
// to AudioSynthHandpan when they have suitable rights to that material.
// Select 48000 for the primary stress test; set to 44100 for an A/B run.
// The output rate is applied before configureHandpan() computes its coefficients.
#ifndef HANDPAN_TEST_SAMPLE_RATE
#define HANDPAN_TEST_SAMPLE_RATE 48000
#endif
#if HANDPAN_TEST_SAMPLE_RATE != 44100 && HANDPAN_TEST_SAMPLE_RATE != 48000
#error "HANDPAN_TEST_SAMPLE_RATE must be 44100 or 48000"
#endif
constexpr uint32_t OUTPUT_SAMPLE_RATE = HANDPAN_TEST_SAMPLE_RATE;

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

// A wide, handpan-inspired range for the load test. Frequencies are adjustable
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

        // Keep the attack below the body level to reduce peak transients.
        if (!handpan.configureNote(pad, modes, 6, attackSample,
                                   ATTACK_SAMPLE_COUNT, ATTACK_SAMPLE_RATE, 0.03f)) {
            Serial.print("Failed to configure pad ");
            Serial.println(pad);
            return false;
        }
    }
    return true;
}

constexpr uint8_t MELODY_SEQUENCE[] = {0, 4, 2, 5, 1, 6, 4, 7, 3, 2, 6, 0, 5, 1, 7, 2};

// A loose, handpan-inspired 96-BPM phrase: eighth-note pulse, varied velocity,
// occasional quiet grace notes and ringing notes from other parts of the pad bank.
// Times and tuning are illustrative rather than a transcription of a real piece.
struct PatternEvent {
    uint16_t atMs;
    uint8_t pad;
    uint8_t velocity;
};
constexpr PatternEvent REALISTIC_PATTERN[] = {
    {0,    0, 104}, {165,  7,  50}, {312,  4, 74}, {626,  2, 88},
    {795, 10,  38}, {938,  6,  66}, {1250, 4, 70}, {1563, 7, 92},
    {1730,12,  36}, {1875, 3,  69}, {2188,  6, 76}, {2500, 0, 96},
    {2650, 8,  42}, {2813, 4,  64}, {3125,  2, 81}, {3438, 7, 74},
    {3750, 5,  72}, {4063, 1,  60}, {4375,  6, 84}, {4688, 3, 68},
    {5000, 0,  98}, {5165, 9,  40}, {5313, 4, 63}, {5625, 2, 78},
    {5938, 5,  70}, {6250, 7,  90}, {6400,13,  34}, {6563, 3, 66},
    {6875, 6,  80}, {7188, 1,  58}, {7500, 4, 72}, {7813, 2, 82}
};

constexpr uint32_t PHASE_DURATION_MS = 8000;
constexpr uint16_t MELODY_STEP_MS = 260;
constexpr uint16_t ALL_PADS_STEP_MS = 62;
constexpr uint16_t CHORD_STRESS_STEP_MS = 24;
constexpr uint16_t DENSE_PAD_STEP_MS = 12;

enum class TestPhase : uint8_t {
    MELODY,
    REALISTIC_PATTERN,
    ALL_PADS_RINGING,
    RAPID_CHORDS,
    DENSE_16_PAD_LOAD
};

TestPhase testPhase = TestPhase::MELODY;
uint32_t phaseStartedAt = 0;
uint32_t nextStepAt = 0;
uint32_t nextStatsAt = 0;
uint8_t stepIndex = 0;
uint32_t droppedEvents = 0;

const char *phaseName(TestPhase phase)
{
    switch (phase) {
    case TestPhase::MELODY: return "MELODY + HALL";
    case TestPhase::REALISTIC_PATTERN: return "REALISTIC HANDPAN PATTERN";
    case TestPhase::ALL_PADS_RINGING: return "ALL 16 PADS / BODY TAILS";
    case TestPhase::RAPID_CHORDS: return "RAPID 4-NOTE BURSTS";
    case TestPhase::DENSE_16_PAD_LOAD: return "DENSE 16-PAD SWEEP / BODY LOAD";
    default: return "UNKNOWN";
    }
}

void resetUsagePeaks()
{
    AudioProcessorUsageMaxReset();
    handpan.AudioProcessorUsageMaxReset();
    hall.AudioProcessorUsageMaxReset();
    AudioMemoryUsageMaxReset();
}

void enterPhase(TestPhase phase, uint32_t now)
{
    testPhase = phase;
    phaseStartedAt = now;
    stepIndex = 0;
    nextStepAt = (phase == TestPhase::REALISTIC_PATTERN) ? now : now + 20;
    resetUsagePeaks();

    Serial.print("\n--- Handpan load-test phase: ");
    Serial.print(phaseName(testPhase));
    Serial.println(" ---");
}

void playMelodyStep()
{
    const uint8_t slot = MELODY_SEQUENCE[stepIndex % (sizeof(MELODY_SEQUENCE) / sizeof(MELODY_SEQUENCE[0]))];
    const uint8_t velocity = (stepIndex % 4U == 0U) ? 100 : 76;
    if (!handpan.noteOn(slot, velocity)) {
        ++droppedEvents;
    }

    if (stepIndex == 4 || stepIndex == 10 || stepIndex == 14) {
        const uint8_t harmony = (uint8_t)((slot + 3U) % PAD_COUNT);
        if (!handpan.noteOn(harmony, 56)) {
            ++droppedEvents;
        }
    }
    ++stepIndex;
}

void playRealisticPatternStep()
{
    constexpr size_t PATTERN_COUNT = sizeof(REALISTIC_PATTERN) / sizeof(REALISTIC_PATTERN[0]);
    if (stepIndex >= PATTERN_COUNT) {
        nextStepAt = phaseStartedAt + PHASE_DURATION_MS;
        return;
    }

    const PatternEvent &event = REALISTIC_PATTERN[stepIndex];
    if (!handpan.noteOn(event.pad, event.velocity)) {
        ++droppedEvents;
    }
    ++stepIndex;

    // Use the phase-relative schedule instead of accumulating loop jitter.
    nextStepAt = (stepIndex < PATTERN_COUNT)
                     ? phaseStartedAt + REALISTIC_PATTERN[stepIndex].atMs
                     : phaseStartedAt + PHASE_DURATION_MS;
}

void playAllPadsStep()
{
    // Strike every one of the 16 pads once per roughly one-second sweep.
    // Tails from earlier pads remain active while later pads are struck.
    const uint8_t slot = (uint8_t)(stepIndex % PAD_COUNT);
    const uint8_t velocity = (uint8_t)(62U + ((stepIndex % 4U) * 4U));
    if (!handpan.noteOn(slot, velocity)) {
        ++droppedEvents;
    }
    ++stepIndex;
}

void playDensePadLoadStep()
{
    // Four notes every 12 ms visits all 16 pads in 36 ms. In the low-CPU preset
    // the upper modes are disabled, so this stresses overlap of the 48 body modes.
    // This is deliberately synthetic worst-case material, not a musical phrase.
    for (uint8_t i = 0; i < 4; ++i) {
        const uint8_t slot = (uint8_t)(((stepIndex * 4U) + i) % PAD_COUNT);
        const uint8_t velocity = (uint8_t)(28U + (((stepIndex + i) % 4U) * 3U));
        if (!handpan.noteOn(slot, velocity)) {
            ++droppedEvents;
        }
    }
    ++stepIndex;
}

void playRapidChordStep()
{
    // Four distinct pads every 24 ms stresses the event queue and reaches the
    // eight overlapping sample-voice limit, while resonances from all pads ring.
    static constexpr uint8_t OFFSETS[4] = {0, 5, 10, 15};
    for (uint8_t i = 0; i < 4; ++i) {
        const uint8_t slot = (uint8_t)(((stepIndex * 4U) + OFFSETS[i]) % PAD_COUNT);
        const uint8_t velocity = (uint8_t)(38U + i * 2U);
        if (!handpan.noteOn(slot, velocity)) {
            ++droppedEvents;
        }
    }
    ++stepIndex;
}

void printStatistics(uint32_t now)
{
    Serial.print("Phase: ");
    Serial.print(phaseName(testPhase));
    Serial.print(" | graph CPU avg/max: ");
    Serial.print(AudioProcessorUsage(), 1);
    Serial.print("/");
    Serial.print(AudioProcessorUsageMax(), 1);
    Serial.print("% | synth avg/max: ");
    Serial.print(handpan.AudioProcessorUsage(), 1);
    Serial.print("/");
    Serial.print(handpan.AudioProcessorUsageMax(), 1);
    Serial.print("% | hall avg/max: ");
    Serial.print(hall.AudioProcessorUsage(), 1);
    Serial.print("/");
    Serial.print(hall.AudioProcessorUsageMax(), 1);
    Serial.print("% | blocks current/max: ");
    Serial.print(AudioMemoryUsage());
    Serial.print("/");
    Serial.print(AudioMemoryUsageMax());
    Serial.print(" | dropped events: ");
    Serial.println(droppedEvents);

    (void)now;
    resetUsagePeaks();
}

} // namespace

void setup()
{
    Serial.begin(115200);
    delay(200);

    Serial.println();
    Serial.println("======================================");
    Serial.println(" Handpan hybrid synth + load test");
    Serial.printf(" Output sample rate: %lu Hz\n", (unsigned long)OUTPUT_SAMPLE_RATE);
    Serial.println("======================================");

    // Set the output rate before AudioMemory() and before configuring the
    // resonators, otherwise their coefficients would still assume 44.1 kHz.
    if (!audioOut.setSampleRate((float)OUTPUT_SAMPLE_RATE)) {
        Serial.printf("ERROR: Could not set output sample rate to %lu Hz\n",
                      (unsigned long)OUTPUT_SAMPLE_RATE);
        while (true) delay(1000);
    }

    // H2 adds a little harmonic roughness without introducing another resonator.
    handpan.setSaturation(0.25f);
    buildExampleAttack();
    if (!configureHandpan()) {
        while (true) delay(1000);
    }

    // A light wet return keeps the handpan strike clear while adding a shared
    // room tail. Freeverb's delay memory is allocated once during setup.
    hall.roomsize(0.68f);
    hall.damping(0.46f);
    const bool hallReady = hall.begin();
    if (!hallReady) {
        Serial.println("WARNING: Freeverb memory allocation failed; continuing dry");
    }

    dryWetMixer.gain(0, 0.72f);              // Dry signal
    dryWetMixer.gain(1, hallReady ? 0.14f : 0.0f); // Wet return

    audioOut.setChannelCount(codec.channels());
    AudioMemory(32);

    bool codecReady = false;
#if defined(AUDIO_CODEC_ES8311)
    // The ES8311 requires its sample-rate-specific clock configuration.
    codecReady = codec.enable(OUTPUT_SAMPLE_RATE);
#else
    // The ES8388 follows the configured I2S clock.
    codecReady = codec.enable();
#endif
    if (!codecReady) {
        Serial.println("ERROR: audio codec initialization failed");
        while (true) delay(1000);
    }
    codec.volume(0.2f);

    const uint32_t now = millis();
    enterPhase(TestPhase::MELODY, now);
    nextStatsAt = now + 1000;
    Serial.println("Handpan hybrid demo + Freeverb load test started");
}

void loop()
{
    const uint32_t now = millis();

    if ((uint32_t)(now - phaseStartedAt) >= PHASE_DURATION_MS) {
        const TestPhase nextPhase =
            testPhase == TestPhase::MELODY ? TestPhase::REALISTIC_PATTERN :
            testPhase == TestPhase::REALISTIC_PATTERN ? TestPhase::ALL_PADS_RINGING :
            testPhase == TestPhase::ALL_PADS_RINGING ? TestPhase::RAPID_CHORDS :
            testPhase == TestPhase::RAPID_CHORDS ? TestPhase::DENSE_16_PAD_LOAD :
            TestPhase::MELODY;
        enterPhase(nextPhase, now);
    }

    if ((int32_t)(now - nextStepAt) >= 0) {
        switch (testPhase) {
        case TestPhase::MELODY:
            nextStepAt = now + MELODY_STEP_MS;
            playMelodyStep();
            break;
        case TestPhase::REALISTIC_PATTERN:
            playRealisticPatternStep();
            break;
        case TestPhase::ALL_PADS_RINGING:
            nextStepAt = now + ALL_PADS_STEP_MS;
            playAllPadsStep();
            break;
        case TestPhase::RAPID_CHORDS:
            nextStepAt = now + CHORD_STRESS_STEP_MS;
            playRapidChordStep();
            break;
        case TestPhase::DENSE_16_PAD_LOAD:
            nextStepAt = now + DENSE_PAD_STEP_MS;
            playDensePadLoadStep();
            break;
        }
    }

    if ((int32_t)(now - nextStatsAt) >= 0) {
        nextStatsAt = now + 1000;
        printStatistics(now);
    }
}
