#include <Arduino.h>
#include <Audio.h>

// Board-specific I2S pins are supplied by the PlatformIO environment.
AudioOutputI2S output({
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

AudioSynthWaveformSine sine;
AudioConnection patchCordLeft(sine, 0, output, 0);
AudioConnection patchCordRight(sine, 0, output, 1);

namespace {
constexpr uint32_t SAMPLE_RATES[] = {22500, 44100, 48000};
constexpr size_t SAMPLE_RATE_COUNT = sizeof(SAMPLE_RATES) / sizeof(SAMPLE_RATES[0]);
constexpr float FIRST_FREQUENCY_HZ = 440.0f;
constexpr float SECOND_FREQUENCY_HZ = 880.0f;
constexpr float TONE_AMPLITUDE = 0.25f;
constexpr uint32_t TONE_DURATION_MS = 1000;
constexpr uint32_t PAUSE_DURATION_MS = 500;

enum class TestPhase : uint8_t {
    FirstTone,
    PauseBeforeFrequencyChange,
    SecondTone,
    PauseBeforeNextSampleRate,
    Done
};

TestPhase phase = TestPhase::Done;
size_t sampleRateIndex = 0;
uint32_t phaseStartedAt = 0;

void enterPhase(TestPhase newPhase) {
    phase = newPhase;
    phaseStartedAt = millis();
}

void startFirstTone() {
    Serial.printf("\nTesting output sample rate: %lu Hz\n",
                  (unsigned long)SAMPLE_RATES[sampleRateIndex]);
    sine.frequency(FIRST_FREQUENCY_HZ);
    sine.amplitude(TONE_AMPLITUDE);
    enterPhase(TestPhase::FirstTone);
}

bool initializeCodecForRate(uint32_t outputRate) {
#if defined(AUDIO_CODEC_ES8311)
    // The ES8311 clock table has no exact 22500-Hz entry. Use the closest
    // supported 22050-Hz clock profile for this deliberately nonstandard test
    // rate; the I2S/AudioStream rate itself remains 22500 Hz.
    const uint32_t codecRate = outputRate == 22500 ? 22050 : outputRate;
    if (outputRate == 22500) {
        Serial.println("ES8311: using 22050-Hz clock profile for 22500-Hz I2S test");
    }
    return codec.enable(codecRate);
#else
    (void)outputRate;
    return codec.enable();
#endif
}

bool changeSampleRate(uint32_t rate) {
    // Reconfigure the I2S clock while the sine output is muted.
    AudioNoInterrupts();
    const bool changed = output.setSampleRate((float)rate);
    AudioInterrupts();

    if (!changed) {
        Serial.printf("ERROR: Could not set sample rate to %lu Hz\n",
                      (unsigned long)rate);
        sine.amplitude(0.0f);
        enterPhase(TestPhase::Done);
        return false;
    }

    if (!initializeCodecForRate(rate)) {
        Serial.printf("ERROR: Could not initialize codec for %lu-Hz output\n",
                      (unsigned long)rate);
        sine.amplitude(0.0f);
        enterPhase(TestPhase::Done);
        return false;
    }

    // Recompute the oscillator phase increment for the new sample rate.
    sine.frequency(FIRST_FREQUENCY_HZ);
    return true;
}
} // namespace

void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println();
    Serial.println("======================================");
    Serial.println(" Runtime Sample-Rate Sine Test");
    Serial.println(" 22500 / 44100 / 48000 Hz");
    Serial.println("======================================");

    // Set the initial rate before AudioMemory() initializes the audio graph.
    if (!output.setSampleRate((float)SAMPLE_RATES[0])) {
        Serial.println("ERROR: Could not set initial sample rate");
        while (true) delay(1000);
    }

    AudioMemory(8);

    if (!initializeCodecForRate(SAMPLE_RATES[0])) {
        Serial.println("ERROR: Audio codec initialization failed");
        while (true) delay(1000);
    }
    codec.volume(0.5f);

    sine.amplitude(0.0f);
    startFirstTone();
}

void loop() {
    const uint32_t elapsed = millis() - phaseStartedAt;

    switch (phase) {
    case TestPhase::FirstTone:
        if (elapsed >= TONE_DURATION_MS) {
            sine.amplitude(0.0f);
            Serial.println("Pause 500 ms");
            enterPhase(TestPhase::PauseBeforeFrequencyChange);
        }
        break;

    case TestPhase::PauseBeforeFrequencyChange:
        if (elapsed >= PAUSE_DURATION_MS) {
            sine.frequency(SECOND_FREQUENCY_HZ);
            sine.amplitude(TONE_AMPLITUDE);
            Serial.printf("Frequency changed to %.0f Hz\n", SECOND_FREQUENCY_HZ);
            enterPhase(TestPhase::SecondTone);
        }
        break;

    case TestPhase::SecondTone:
        if (elapsed >= TONE_DURATION_MS) {
            sine.amplitude(0.0f);
            enterPhase(TestPhase::PauseBeforeNextSampleRate);
        }
        break;

    case TestPhase::PauseBeforeNextSampleRate:
        if (elapsed >= PAUSE_DURATION_MS) {
            ++sampleRateIndex;
            if (sampleRateIndex >= SAMPLE_RATE_COUNT) {
                Serial.println("\nAll sample-rate tests completed.");
                enterPhase(TestPhase::Done);
                break;
            }

            const uint32_t nextRate = SAMPLE_RATES[sampleRateIndex];
            if (changeSampleRate(nextRate)) {
                startFirstTone();
            }
        }
        break;

    case TestPhase::Done:
    default:
        delay(100);
        break;
    }
}
