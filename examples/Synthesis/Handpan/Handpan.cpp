#include <Audio.h>
#include <math.h>

// This quick-start example deliberately generates its attack transient at
// startup, so no third-party recording is bundled with the library. For a
// closer match to the reference prototype, replace this buffer with a licensed
// mono PCM16 attack sample, preferably 20-60 ms long and stored in flash.
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

constexpr float BODY_T60[3] = {2.00f, 1.05f, 0.58f};
constexpr float BODY_RATIOS[3] = {1.0f, 2.0f, 3.0f};
constexpr float METAL_RATIOS[3] = {6.35f, 8.85f, 11.65f};
constexpr float METAL_T60[3] = {0.075f, 0.115f, 0.095f};
constexpr float METAL_WEIGHTS[3] = {1.0f, 0.64f, 0.42f};
constexpr float METAL_PHASES[3] = {0.90f, 2.15f, 0.25f};

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

// A compact, deterministic noise burst plus a short high-frequency ring.
// This is an example placeholder, not a sample from the user's recording.
void buildExampleAttack()
{
    uint32_t randomState = 0x4d595df4U;
    int32_t peak = 0;

    for (uint32_t i = 0; i < ATTACK_SAMPLE_COUNT; ++i) {
        const float t = (float)i / (float)ATTACK_SAMPLE_RATE;
        randomState = randomState * 1664525U + 1013904223U;
        const float noise = ((float)(randomState >> 8) * (1.0f / 8388607.5f)) - 1.0f;
        const float noiseEnvelope = expf(-t / 0.009f) * fminf(1.0f, t / 0.0006f);
        const float ring = sinf(6.28318530718f * 3650.0f * t) * expf(-t / 0.0038f);
        const float value = 0.65f * noise * noiseEnvelope + 0.35f * ring;
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
            modes[mode].gain = 0.45f * BODY_LEVELS[pad][mode];
            modes[mode].decaySeconds = BODY_T60[mode];
            modes[mode].phaseRadians = BODY_PHASES[pad][mode];
        }

        for (uint8_t mode = 0; mode < 3; ++mode) {
            const float detune = 1.0f + 0.0015f * ((int)(pad % 3) - 1) * (float)(mode + 1);
            modes[mode + 3].frequencyHz = f0 * METAL_RATIOS[mode] * detune;
            modes[mode + 3].gain = 0.055f * METAL_WEIGHTS[mode];
            modes[mode + 3].decaySeconds = METAL_T60[mode];
            modes[mode + 3].phaseRadians = METAL_PHASES[mode] + 0.17f * (float)pad;
        }

        // 0.09 is the attack level preferred in the Python reference comparison.
        if (!handpan.configureNote(pad, modes, 6, attackSample,
                                   ATTACK_SAMPLE_COUNT, ATTACK_SAMPLE_RATE, 0.09f)) {
            Serial.print("Failed to configure pad ");
            Serial.println(pad);
            return false;
        }
    }
    return true;
}

constexpr uint8_t MELODY_SEQUENCE[] = {0, 4, 2, 5, 1, 6, 4, 7, 3, 2, 6, 0, 5, 1, 7, 2};
constexpr uint32_t PHASE_DURATION_MS = 8000;
constexpr uint16_t MELODY_STEP_MS = 260;
constexpr uint16_t ALL_PADS_STEP_MS = 62;
constexpr uint16_t CHORD_STRESS_STEP_MS = 24;

enum class TestPhase : uint8_t {
    MELODY,
    ALL_PADS_RINGING,
    RAPID_CHORDS
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
    case TestPhase::ALL_PADS_RINGING: return "ALL 16 PADS / 96 MODES";
    case TestPhase::RAPID_CHORDS: return "RAPID 4-NOTE BURSTS";
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
    nextStepAt = now + 20;
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

    dryWetMixer.gain(0, 0.82f);              // Dry signal
    dryWetMixer.gain(1, hallReady ? 0.22f : 0.0f); // Wet return

    audioOut.setChannelCount(codec.channels());
    AudioMemory(32);

    if (!codec.enable()) {
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
            testPhase == TestPhase::MELODY ? TestPhase::ALL_PADS_RINGING :
            testPhase == TestPhase::ALL_PADS_RINGING ? TestPhase::RAPID_CHORDS :
            TestPhase::MELODY;
        enterPhase(nextPhase, now);
    }

    if ((int32_t)(now - nextStepAt) >= 0) {
        switch (testPhase) {
        case TestPhase::MELODY:
            nextStepAt = now + MELODY_STEP_MS;
            playMelodyStep();
            break;
        case TestPhase::ALL_PADS_RINGING:
            nextStepAt = now + ALL_PADS_STEP_MS;
            playAllPadsStep();
            break;
        case TestPhase::RAPID_CHORDS:
            nextStepAt = now + CHORD_STRESS_STEP_MS;
            playRapidChordStep();
            break;
        }
    }

    if ((int32_t)(now - nextStatsAt) >= 0) {
        nextStatsAt = now + 1000;
        printStatistics(now);
    }
}
