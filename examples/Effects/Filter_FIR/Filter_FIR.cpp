
#include <Arduino.h>
#include <Audio.h>

constexpr short FIR_COEFFICIENTS[] = {
    1024, 2048, 4096, 9216, 9216, 4096, 2048, 1024,
};

constexpr float NOISE_AMPLITUDE = 0.25f;
constexpr float BIQUAD_CUTOFF_HZ = 500.0f;

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

AudioSynthNoiseWhite noise;
AudioFilterFIR fir;
AudioFilterBiquad biquad;

AudioConnection noiseToFir(noise, 0, fir, 0);
AudioConnection noiseToBiquad(noise, 0, biquad, 0);
AudioConnection firToLeft(fir, 0, output, 0);
AudioConnection biquadToRight(biquad, 0, output, 1);

// ============================================================================
// Setup
// ============================================================================

void setup() {
    Serial.begin(115200);
    delay(1000);
    AudioMemory(16);

    Serial.println();
    Serial.println("======================================");
    Serial.println(" White noise: FIR left / BiQuad right");
    Serial.println("======================================");

    if (!codec.enable()) {
        Serial.println("ERROR: Codec initialization failed");
        while (true)
            delay(1000);
    }

    fir.begin(FIR_COEFFICIENTS,
              sizeof(FIR_COEFFICIENTS) / sizeof(FIR_COEFFICIENTS[0]));
    biquad.setLowpass(0, BIQUAD_CUTOFF_HZ);

    noise.amplitude(NOISE_AMPLITUDE);
    Serial.println("Test started: FIR output left, BiQuad output right.");
}

// ============================================================================
// Loop
// ============================================================================

void loop() {
    static uint32_t lastStatus = 0;

    if (millis() - lastStatus >= 1000) {
        lastStatus = millis();

        Serial.printf("Noise %.0f%% | FIR %.2f%% | BiQuad %.2f%% | "
                      "I2S %.2f%% | Audio blocks %u/%u\n",
                      NOISE_AMPLITUDE * 100.0f, fir.AudioProcessorUsage(),
                      biquad.AudioProcessorUsage(), output.AudioProcessorUsage(),
                      AudioStream::AudioMemoryUsage(),
                      AudioStream::AudioMemoryUsageMax());
    }
}
