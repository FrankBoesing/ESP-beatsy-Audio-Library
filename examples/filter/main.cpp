
#include <Arduino.h>
#include <Audio.h>

namespace {
constexpr short FIR_COEFFICIENTS[] = {
    1024, 2048, 4096, 9216, 9216, 4096, 2048, 1024,
};

constexpr uint32_t AUDIO_MEMORY_BLOCKS = 16;
constexpr float NOISE_AMPLITUDE = 0.25f;
constexpr float BIQUAD_CUTOFF_HZ = 500.0f;
} // namespace

AudioSynthNoiseWhite noise;
AudioFilterFIR fir;
AudioFilterBiquad biquad;

AudioOutputI2S i2s({
    27, // BCLK
    25, // WS / LRCLK
    26, // DOUT
    0   // MCLK
});

AudioControlES8388 codec;

AudioConnection noiseToFir(noise, 0, fir, 0);
AudioConnection noiseToBiquad(noise, 0, biquad, 0);
AudioConnection firToLeft(fir, 0, i2s, 0);
AudioConnection biquadToRight(biquad, 0, i2s, 1);

// ============================================================================
// Setup
// ============================================================================

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("======================================");
    Serial.println(" White noise: FIR left / BiQuad right");
    Serial.println("======================================");

    if (!codec.enable()) {
        Serial.println("ERROR: ES8388 initialization failed");
        while (true)
            delay(1000);
    }

    fir.begin(FIR_COEFFICIENTS,
              sizeof(FIR_COEFFICIENTS) / sizeof(FIR_COEFFICIENTS[0]));
    biquad.setLowpass(0, BIQUAD_CUTOFF_HZ);

    AudioMemory(AUDIO_MEMORY_BLOCKS);

    if (!i2s.begin()) {
        Serial.println("ERROR: I2S initialization failed");
        while (true)
            delay(1000);
    }

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
                      NOISE_AMPLITUDE * 100.0f, fir.processorUsage(),
                      biquad.processorUsage(), i2s.processorUsage(),
                      AudioStream::memoryUsage(),
                      AudioStream::memoryUsageMax());
    }
}
