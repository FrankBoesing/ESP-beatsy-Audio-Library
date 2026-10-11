/*
 * Compare AudioOutputPDM against PDMDirectIDF.cpp.
 *
 * Uses the same two DATA GPIOs, PCM sample rate, sine frequency and amplitude
 * as the low-level IDF test, but routes samples through AudioStream.
 *
 * GPIO 43 = left/DOUT2, GPIO 44 = right/DOUT.
 */

#include <Audio.h>

constexpr int PDM_DATA_LEFT = 43;
constexpr int PDM_DATA_RIGHT = 44;

AudioSynthWaveformSine sine;
AudioOutputPDM pdmOut(AudioOutputPDM::Pins{
    static_cast<int8_t>(PDM_DATA_LEFT),
    static_cast<int8_t>(PDM_DATA_RIGHT)
});

// Send the same sine source to both inputs to match the successful IDF test.
AudioConnection patchLeft(sine, 0, pdmOut, 0);
AudioConnection patchRight(sine, 0, pdmOut, 1);

void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println();
    Serial.println("AudioStream -> AudioOutputPDM stereo test");
    Serial.println("GPIO 43 = left/DOUT2, GPIO 44 = right/DOUT");

    if (!pdmOut.setSampleRate(44100.0f)) {
        Serial.println("setSampleRate failed");
        while (true) delay(1000);
    }

    sine.frequency(1000.0f);
    sine.amplitude(0.20f);
    AudioMemory(16);

    if (!pdmOut.begin()) {
        Serial.println("AudioOutputPDM begin failed");
        while (true) delay(1000);
    }

    Serial.println("PDM test started");
}

void loop() {
    static uint32_t lastReportMs = 0;
    const uint32_t now = millis();

    if (now - lastReportMs >= 1000) {
        lastReportMs = now;
        Serial.printf("TX callbacks: %lu\n",
                      static_cast<unsigned long>(pdmOut.txCallbackCount()));
    }
}
