/*
 * USB Audio loopback test for the Freenove FNK0104B (ESP32-S3).
 *
 * The host playback stream is routed back to the host capture stream.
 * Select this device as the computer's audio output and select its
 * microphone/capture endpoint to verify both directions.
 *
 * Requires an Arduino-ESP32 core that provides USBAudioCard with TinyUSB
 * Audio enabled. The library starts the USB device after registering the
 * audio interface; USB CDC remains configured for Serial.println().
 */

#include <Arduino.h>
#include <Audio.h>

AudioInputUSB usbInput(2);
AudioOutputUSB usbOutput(2);

AudioConnection leftLoopback(usbInput, 0, usbOutput, 0);
AudioConnection rightLoopback(usbInput, 1, usbOutput, 1);

void setup() {
    Serial.begin(115200);

    // Set the graph and USB descriptor rate before AudioMemory() starts
    // hardware initialization. This also sets the rate used by other nodes.
    if (!usbInput.setSampleRate(48000.0f)) {
        Serial.println("Could not set audio sample rate");
    }

    AudioMemory(24);

    delay(500);
    Serial.println("USB Audio loopback initialized");
    Serial.printf("Audio input running: %s\n", usbInput.isRunning() ? "yes" : "no");
    Serial.printf("Audio output running: %s\n", usbOutput.isRunning() ? "yes" : "no");
}

void loop() {
    static uint32_t lastReport = 0;
    if (millis() - lastReport >= 1000) {
        lastReport = millis();
        Serial.printf("USB input streaming: %s, output streaming: %s\n",
                      usbInput.isStreaming() ? "yes" : "no",
                      usbOutput.isStreaming() ? "yes" : "no");
    }
}
