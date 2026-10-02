
#include <Arduino.h>
#include <SD_MMC.h>
#include <Audio.h>
#include "play_mp3.h"
#include "output_i2s.h"
#include "control_es8388.h"


// ============================================================================
// Audio objects
// ============================================================================

AudioControlES8388 codec;
AudioSourceFile source;
AudioPlayAac aac;

AudioOutputI2S i2s({
    27, // BCLK
    25, // WS / LRCLK
    26, // DOUT
    0   // MCLK
});



// ============================================================================
// Connections
// ============================================================================

AudioConnection patchCord1(aac, 0, i2s, 0);
AudioConnection patchCord2(aac, 1, i2s, 1);

// ============================================================================
// Setup
// ============================================================================

void setup() {
    Serial.begin(115200);

    delay(1000);

    Serial.println();
    Serial.println("======================================");
    Serial.println(" AudioPlayAac SD_MMC TEST");
    Serial.println("======================================");

    // ------------------------------------------------------------------------
    // Audio memory
    // ------------------------------------------------------------------------

    AudioMemory(12);

    Serial.println("Audio memory initialized");

    // ------------------------------------------------------------------------
    // ES8388
    // ------------------------------------------------------------------------

    Serial.println("Initializing ES8388...");

    if (!codec.enable()) {
        Serial.println("ERROR: ES8388 initialization failed");

        while (true) {
            delay(1000);
        }
    }

    Serial.println("ES8388 initialized");

    float volume = 0.7f;
    codec.volume(volume);
    Serial.print("Volume:");
    Serial.println(volume, 2);

    // ------------------------------------------------------------------------
    // SD_MMC
    // ------------------------------------------------------------------------

    Serial.println("Initializing SD_MMC...");

    /*
     * ESP32 Audio Kit V2.2
     *
     * 1-bit mode.
     */
    if (!SD_MMC.begin("/sdcard", false)) {
        Serial.println("ERROR: SD_MMC initialization failed");

        while (true) {
            delay(1000);
        }
    }

    Serial.println("SD_MMC initialized");

    // ------------------------------------------------------------------------
    // Open MP3
    // ------------------------------------------------------------------------

    Serial.println("Opening /test.aac...");

    if (!source.open(SD_MMC, "/test.aac")) {
        Serial.println("ERROR: Could not open /test.aac");

        while (true) {
            delay(1000);
        }
    }

    Serial.printf("AAC source opened, size=%llu bytes\n",
                  (unsigned long long)source.size());

    // ------------------------------------------------------------------------
    // Start playback
    // ------------------------------------------------------------------------

    Serial.println("Starting AAC playback...");

    if (!aac.play(source)) {
        Serial.println("ERROR: AAC playback could not be started");

        while (true) {
            delay(1000);
        }
    }

    Serial.println("AAC playback started");
}

// ============================================================================
// Loop
// ============================================================================

void loop() {
    static uint32_t lastStatus = 0;

    if (millis() - lastStatus >= 1000) {
        lastStatus = millis();

        Serial.println();
        Serial.println("--------------------------------------");

        Serial.printf("Playing: %s\n", aac.isPlaying() ? "yes" : "no");

        /*
         * AudioStream statistics:
         *   processorUsage()    = most recent update() execution
         *   processorUsageMax() = maximum update() execution since start
         */
        Serial.printf("Audio CPU: AAC %.2f%% (max %.2f%%), "
                      "I2S %.2f%% (max %.2f%%)\n",
                      aac.processorUsage(), aac.processorUsageMax(),
                      i2s.processorUsage(), i2s.processorUsageMax());

        Serial.printf("Audio memory: %u / %u blocks "
                      "(current / max)\n",
                      AudioStream::memoryUsage(),
                      AudioStream::memoryUsageMax());

        /*
         * AAC-specific decoder load:
         * total time inside MP3Decode(), relative to the audio time generated.
         * Source/SD waiting and vTaskDelay() are deliberately excluded.
         */
#if SOFTCODEC_METRICS
        Serial.printf("AAC decode: avg %.2f%%, frame max %.2f%%, "
                      "frames %lu, decode %.3f s\n",
                      aac.decodeProcessorUsage(), aac.decodeProcessorUsageMax(),
                      (unsigned long)aac.decodeFrames(),
                      (double)aac.decodeTimeUsTotal() / 1000000.0);
#endif
        Serial.printf("Position: %lu ms / %lu ms\n",
                      (unsigned long)aac.positionMillis(),
                      (unsigned long)aac.lengthMillis());
    }
}
