/*
 * USB Audio loopback test for the Freenove FNK0104B (ESP32-S3).
 *
 * The host playback stream is routed back to the host capture stream.
 * Select this device as the computer's audio output and select its
 * microphone/capture endpoint to verify both directions.
 *
 * Requires an Arduino-ESP32 core that provides USBAudioCard with TinyUSB
 * Audio enabled. CDC-on-boot must be disabled so the audio interface can be
 * registered before USB.begin(). The backend then starts CDC + Audio together;
 * use USBSerial for logging over the same USB cable.
 */

#include <Arduino.h>
#include <Audio.h>

AudioInputUSB usbInput(2);
AudioOutputUSB usbOutput(2);

AudioConnection leftLoopback(usbInput, 0, usbOutput, 0);
AudioConnection rightLoopback(usbInput, 1, usbOutput, 1);

void setup() {
    // Set the graph and USB descriptor rate before AudioMemory() starts
    // hardware initialization. This also sets the rate used by other nodes.
    const bool sampleRateSet = usbInput.setSampleRate(48000.0f);

    AudioMemory(24);

    // Explicitly start both nodes. This registers UAC and CDC before the
    // backend starts TinyUSB; repeated begin() calls are safe.
    const bool inputStarted = usbInput.begin();
    const bool outputStarted = usbOutput.begin();

    delay(500);
    if (!sampleRateSet) {
        USBSerial.println("Could not set audio sample rate");
    }
    USBSerial.println("USB Audio loopback initialized");
    USBSerial.printf("Audio input begin: %s, running: %s\n",
                     inputStarted ? "ok" : "failed",
                     usbInput.isRunning() ? "yes" : "no");
    USBSerial.printf("Audio output begin: %s, running: %s\n",
                     outputStarted ? "ok" : "failed",
                     usbOutput.isRunning() ? "yes" : "no");
}

void loop() {
    static uint32_t lastReport = 0;
    if (millis() - lastReport >= 1000) {
        lastReport = millis();
        USBSerial.printf("USB input streaming: %s, output streaming: %s\n",
                      usbInput.isStreaming() ? "yes" : "no",
                      usbOutput.isStreaming() ? "yes" : "no");
    }
}
