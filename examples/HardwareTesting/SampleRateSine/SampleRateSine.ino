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

constexpr uint32_t SAMPLE_RATES[] = {32000, 44100, 48000};
constexpr size_t SAMPLE_RATE_COUNT = sizeof(SAMPLE_RATES) / sizeof(SAMPLE_RATES[0]);
constexpr float TEST_FREQUENCY_HZ = 1000.0f;
constexpr float TONE_AMPLITUDE = 0.25f;
constexpr uint32_t TONE_DURATION_MS = 1000;
constexpr uint32_t PAUSE_DURATION_MS = 500;

enum class TestPhase : uint8_t {
    TestTone,
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

void startTestTone() {
    Serial.printf("\nTesting output sample rate: %lu Hz, tone: %.0f Hz\n",
                  (unsigned long)SAMPLE_RATES[sampleRateIndex], TEST_FREQUENCY_HZ);
    sine.frequency(TEST_FREQUENCY_HZ);
    sine.amplitude(TONE_AMPLITUDE);
    enterPhase(TestPhase::TestTone);
}

bool initializeCodecForRate(uint32_t outputRate) {
    bool initialized = false;
#if defined(AUDIO_CODEC_ES8311)
    // The ES8311 has explicit clock-table entries for each tested rate.
    initialized = codec.enable(outputRate);
#else
    (void)outputRate;
    initialized = codec.enable();
#endif
    // Codec initialization restores its default volume, so reapply the test
    // level after every sample-rate switch.
    return initialized && codec.volume(0.2f);
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

#if defined(AUDIO_CODEC_ES8311)
    // The ES8311 has sample-rate-specific clock divider settings.
    if (!initializeCodecForRate(rate)) {
        Serial.printf("ERROR: Could not initialize codec for %lu-Hz output\n",
                      (unsigned long)rate);
        sine.amplitude(0.0f);
        enterPhase(TestPhase::Done);
        return false;
    }
#else
    // The ES8388 uses fixed MCLK/LRCK ratios and follows the I2S clock.
    // Do not reset it on every rate change or call Wire.begin() again.
    (void)rate;
#endif

    // Recompute the oscillator phase increment for the new sample rate.
    sine.frequency(TEST_FREQUENCY_HZ);
    return true;
}

void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println();
    Serial.println("======================================");
    Serial.println(" Runtime Sample-Rate Sine Test");
    Serial.println(" 32000 / 44100 / 48000 Hz");
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

    sine.amplitude(0.0f);
    startTestTone();
}

void loop() {
    const uint32_t elapsed = millis() - phaseStartedAt;

    switch (phase) {
    case TestPhase::TestTone:
        if (elapsed >= TONE_DURATION_MS) {
            sine.amplitude(0.0f);
            Serial.println("Pause 500 ms");
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
                startTestTone();
            }
        }
        break;

    case TestPhase::Done:
    default:
        delay(100);
        break;
    }
}
