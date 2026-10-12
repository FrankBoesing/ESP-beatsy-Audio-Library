/*
 * S/PDIF output diagnostic for ESP32-S3 / ESP-IDF Arduino.
 *
 * Copy this file to your application's src/main.cpp (or integrate it into
 * your own test project). Set SPDIF_DATA_PIN to the GPIO wired to your
 * S/PDIF transmitter/interface.
 *
 * The program generates a continuous 1 kHz sine at 44.1 kHz and reports
 * AudioOutputSPDIF queue, DMA-callback, encoding and write diagnostics.
 */

#include <Arduino.h>
#include <Audio.h>

#ifndef SPDIF_DATA_PIN
#define SPDIF_DATA_PIN 18  // Change to the GPIO actually wired for S/PDIF DATA.
#endif

namespace {
constexpr float TEST_SAMPLE_RATE_HZ = 44100.0f;
constexpr float TEST_TONE_HZ = 1000.0f;
constexpr float TEST_AMPLITUDE = 0.25f;

AudioSynthWaveformSine testTone;
AudioOutputSPDIF spdifOut(static_cast<int8_t>(SPDIF_DATA_PIN));
AudioConnection patchLeft(testTone, 0, spdifOut, 0);
AudioConnection patchRight(testTone, 0, spdifOut, 1);

uint32_t lastReportMs = 0;
uint32_t previousCallbacks = 0;
uint32_t previousBlocks = 0;
uint32_t previousDrops = 0;
uint32_t previousWriteErrors = 0;

void stopOnFatalError(const char *message) {
    Serial.println(message);
    while (true) {
        delay(1000);
    }
}

void reportDiagnostics() {
    const uint32_t now = millis();
    const uint32_t elapsedMs = now - lastReportMs;
    if (elapsedMs < 1000) return;
    lastReportMs = now;

    const uint32_t callbacks = spdifOut.txCallbackCount();
    const uint32_t blocks = spdifOut.txEncodedBlockCount();
    const uint32_t drops = spdifOut.txQueueDropCount();
    const uint32_t errors = spdifOut.txWriteErrorCount();

    const uint32_t callbackRate =
        static_cast<uint32_t>((static_cast<uint64_t>(callbacks - previousCallbacks) * 1000U) / elapsedMs);
    const uint32_t blockRate =
        static_cast<uint32_t>((static_cast<uint64_t>(blocks - previousBlocks) * 1000U) / elapsedMs);

    Serial.printf(
        "SPDIF running=%u | callback=%lu/s (total %lu) | "
        "written=%lu/s (total %lu) | queueDrops=+%lu (total %lu) | "
        "writeErrors=+%lu (total %lu) | maxEncode=%lu us | maxWrite=%lu us\n",
        spdifOut.isRunning() ? 1U : 0U,
        static_cast<unsigned long>(callbackRate),
        static_cast<unsigned long>(callbacks),
        static_cast<unsigned long>(blockRate),
        static_cast<unsigned long>(blocks),
        static_cast<unsigned long>(drops - previousDrops),
        static_cast<unsigned long>(drops),
        static_cast<unsigned long>(errors - previousWriteErrors),
        static_cast<unsigned long>(errors),
        static_cast<unsigned long>(spdifOut.maxEncodeTimeUs()),
        static_cast<unsigned long>(spdifOut.maxWriteTimeUs()));

    previousCallbacks = callbacks;
    previousBlocks = blocks;
    previousDrops = drops;
    previousWriteErrors = errors;
}
} // namespace

void setup() {
    Serial.begin(115200);
    const uint32_t serialWaitStart = millis();
    while (!Serial && (millis() - serialWaitStart < 3000U)) {
        delay(10);
    }

    Serial.println();
    Serial.println("AudioOutputSPDIF diagnostic");
    Serial.printf("DATA GPIO: %d\n", SPDIF_DATA_PIN);
    Serial.printf("PCM rate: %.0f Hz; test tone: %.0f Hz\n",
                  TEST_SAMPLE_RATE_HZ, TEST_TONE_HZ);
    Serial.println("Use a proper optical transmitter or S/PDIF line interface; do not drive coax directly from GPIO.");

    AudioMemory(16);

    if (!spdifOut.setSampleRate(TEST_SAMPLE_RATE_HZ)) {
        stopOnFatalError("ERROR: spdifOut.setSampleRate() failed");
    }

    testTone.frequency(TEST_TONE_HZ);
    testTone.amplitude(TEST_AMPLITUDE);

    if (!spdifOut.begin()) {
        stopOnFatalError("ERROR: spdifOut.begin() failed; check the DATA GPIO and I2S0 availability");
    }

    previousCallbacks = spdifOut.txCallbackCount();
    previousBlocks = spdifOut.txEncodedBlockCount();
    previousDrops = spdifOut.txQueueDropCount();
    previousWriteErrors = spdifOut.txWriteErrorCount();
    lastReportMs = millis();

    Serial.println("Playback started. Keep this test running while listening for dropouts.");
}

void loop() {
    reportDiagnostics();
    delay(1);
}
