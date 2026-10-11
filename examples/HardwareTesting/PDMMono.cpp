/*
 * Hardware smoke test for AudioOutputPDM.
 *
 * Wire PDM_DATA_PIN through a simple RC low-pass (for example 1 kOhm + 4.7 nF)
 * to an amplifier input. Do not connect a speaker directly to the ESP32 GPIO.
 * Change the pin below to a GPIO that is free on your board.
 */

#include <Audio.h>

constexpr int PDM_DATA_PIN = 17; // Select a free output-capable GPIO for your board.

AudioSynthWaveformSine sine;
AudioOutputPDM pdmOut(PDM_DATA_PIN);
AudioConnection patchCord(sine, 0, pdmOut, 0);

void setup() {
    Serial.begin(115200);
    delay(500);

    AudioMemory(12);
    sine.frequency(1000.0f);
    sine.amplitude(0.25f);

    if (!pdmOut.setSampleRate(44100.0f)) {
        Serial.println("PDM sample-rate setup failed");
    }

    if (!pdmOut.begin()) {
        Serial.println("PDM start failed. Check target support and DATA pin.");
        while (true) delay(1000);
    }

    Serial.println("PDM 1 kHz mono test started");
}

void loop() {
    static uint32_t lastStatus = 0;
    const uint32_t now = millis();

    if (now - lastStatus >= 1000) {
        lastStatus = now;
        Serial.printf("PDM TX callbacks: %lu\n",
                      static_cast<unsigned long>(pdmOut.txCallbackCount()));
    }
}
