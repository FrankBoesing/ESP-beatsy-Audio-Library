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
AudioConnection handpanToLeft(handpan, 0, audioOut, 0);
AudioConnection handpanToRight(handpan, 0, audioOut, 1);

namespace {

constexpr uint8_t PAD_COUNT = 8;
constexpr float BODY_T60[3] = {2.00f, 1.05f, 0.58f};
constexpr float BODY_RATIOS[3] = {1.0f, 2.0f, 3.0f};
constexpr float METAL_RATIOS[3] = {6.35f, 8.85f, 11.65f};
constexpr float METAL_T60[3] = {0.075f, 0.115f, 0.095f};
constexpr float METAL_WEIGHTS[3] = {1.0f, 0.64f, 0.42f};
constexpr float METAL_PHASES[3] = {0.90f, 2.15f, 0.25f};

// Preliminary pitch estimates from the supplied recording. These are working
// values for this demo, not a validated tuning file for a specific instrument.
constexpr float PAD_FREQUENCIES[PAD_COUNT] = {
    139.6f, 209.4f, 246.5f, 277.1f, 312.6f, 330.7f, 372.3f, 415.7f
};
constexpr float BODY_LEVELS[PAD_COUNT][3] = {
    {0.70f, 0.10f, 0.30f}, {0.65f, 0.42f, 0.16f},
    {0.70f, 0.55f, 0.20f}, {0.50f, 0.75f, 0.30f},
    {0.58f, 0.68f, 0.38f}, {0.62f, 0.24f, 0.18f},
    {0.65f, 0.23f, 0.17f}, {0.72f, 0.32f, 0.20f}
};
constexpr float BODY_PHASES[PAD_COUNT][3] = {
    {0.12f, 1.10f, 2.05f}, {0.52f, 1.75f, 0.35f},
    {0.40f, 2.05f, 1.30f}, {0.76f, 0.60f, 2.60f},
    {1.12f, 1.45f, 0.90f}, {0.35f, 2.20f, 1.85f},
    {0.62f, 1.00f, 2.40f}, {0.92f, 2.55f, 1.50f}
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

constexpr uint8_t SEQUENCE[] = {0, 4, 2, 5, 1, 6, 4, 7, 3, 2, 6, 0, 5, 1, 7, 2};
constexpr uint16_t STEP_MS = 260;
uint32_t nextStepAt = 0;
uint32_t nextStatsAt = 0;
uint8_t stepIndex = 0;

void playStep()
{
    const uint8_t slot = SEQUENCE[stepIndex];
    const uint8_t velocity = (stepIndex % 4U == 0U) ? 108 : 82;
    if (!handpan.noteOn(slot, velocity)) {
        Serial.println("Handpan event queue full");
    }

    // Deliberately strike a second pad on selected steps. The two pitches and
    // all older tails overlap; the per-pad modal states are not reset.
    if (stepIndex == 4 || stepIndex == 10 || stepIndex == 14) {
        const uint8_t harmony = (uint8_t)((slot + 3U) % PAD_COUNT);
        handpan.noteOn(harmony, 60);
    }

    stepIndex = (uint8_t)((stepIndex + 1U) % (sizeof(SEQUENCE) / sizeof(SEQUENCE[0])));
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

    audioOut.setChannelCount(codec.channels());
    AudioMemory(20);

    if (!codec.enable()) {
        Serial.println("ERROR: audio codec initialization failed");
        while (true) delay(1000);
    }
    codec.volume(0.2f);

    nextStepAt = millis() + 300;
    nextStatsAt = millis() + 5000;
    Serial.println("Handpan hybrid demo started (procedural attack sample)");
}

void loop()
{
    const uint32_t now = millis();
    if ((int32_t)(now - nextStepAt) >= 0) {
        nextStepAt += STEP_MS;
        if ((int32_t)(now - nextStepAt) >= (int32_t)STEP_MS) {
            nextStepAt = now + STEP_MS;
        }
        playStep();
    }

    if ((int32_t)(now - nextStatsAt) >= 0) {
        nextStatsAt = now + 5000;
        Serial.print("Audio CPU: ");
        Serial.print(AudioProcessorUsage(), 1);
        Serial.print("% (max ");
        Serial.print(AudioProcessorUsageMax(), 1);
        Serial.print("%), memory: ");
        Serial.print(AudioMemoryUsage());
        Serial.print(" blocks (max ");
        Serial.print(AudioMemoryUsageMax());
        Serial.println(")");
        AudioProcessorUsageMaxReset();
        AudioMemoryUsageMaxReset();
    }
}
