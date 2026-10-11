/*
 * Diagnostic for the AudioOutputPDM MONO mixing/queue path.
 *
 * One physical DATA line on GPIO 43, but the same sine is connected to BOTH
 * AudioOutputPDM inputs. The class should average identical blocks and produce
 * exactly the same mono PCM signal as the one-input variant.
 *
 * Compare this with PDMMono.cpp, which connects the source only to input 0.
 */

#include <Audio.h>

constexpr int PDM_DATA_PIN = 43;

AudioSynthWaveformSine sine;
AudioOutputPDM pdmOut(PDM_DATA_PIN);
AudioConnection patchLeft(sine, 0, pdmOut, 0);
AudioConnection patchRight(sine, 0, pdmOut, 1);

void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println();
    Serial.println("AudioStream -> AudioOutputPDM MONO, both inputs connected");
    Serial.println("DATA: GPIO 43");

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

    Serial.println("PDM mono dual-input test started");
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
