/*
 * Hardware smoke test for AudioOutputPDM.
 *
 * PDM is a high-frequency 1-bit stream. Do not connect it directly to a
 * speaker or amplifier input. Use this two-stage passive low-pass:
 *
 * DATA -- 1 kOhm --+-- 1 kOhm --+--> amplifier input
 *                  |             |
 *                2.2 nF        2.2 nF
 *                  |             |
 *                 GND           GND
 *
 * Join ESP32 ground to amplifier ground. This is a starting point; a buffer
 * or active filter may be needed depending on the amplifier input.
 * Change the pin below to a GPIO that is free on your board.
 */

#include <Audio.h>

constexpr int PDM_DATA_PIN = 43; // Same physical DATA pin as the successful direct-IDF test.

AudioSynthWaveformSine sine;
AudioOutputPDM pdmOut(PDM_DATA_PIN);
AudioConnection patchCord(sine, 0, pdmOut, 0);

void setup() {
    Serial.begin(115200);
    delay(500);

    if (!pdmOut.setSampleRate(44100.0f)) {
        Serial.println("PDM sample-rate setup failed");
    }

    sine.frequency(1000.0f);
    sine.amplitude(0.20f);
    AudioMemory(12);

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
