#include <Arduino.h>
#include <SD_MMC.h>

#include "AudioStream.h"
#include "AudioSourceFile.h"
#include "play_mp3.h"
#include "output_i2s.h"
#include "control_es8388.h"


// ============================================================================
// Audio objects
// ============================================================================

AudioSourceFile source;

AudioPlayMp3 mp3;

AudioOutputI2S i2s({
    27,     // BCLK
    25,     // WS / LRCLK
    26,     // DOUT
    0       // MCLK
});

AudioControlES8388 codec;


// ============================================================================
// Connections
// ============================================================================

AudioConnection patchCord1(
    mp3,
    0,
    i2s,
    0
);

AudioConnection patchCord2(
    mp3,
    0,
    i2s,
    1
);


// ============================================================================
// Setup
// ============================================================================

void setup()
{
    Serial.begin(115200);

    delay(1000);

    Serial.println();
    Serial.println("======================================");
    Serial.println(" AudioPlayMp3 SD_MMC TEST");
    Serial.println("======================================");


    // ------------------------------------------------------------------------
    // Audio memory
    // ------------------------------------------------------------------------

    AudioMemory(64);

    Serial.println(
        "Audio memory initialized"
    );


    // ------------------------------------------------------------------------
    // ES8388
    // ------------------------------------------------------------------------

    Serial.println(
        "Initializing ES8388..."
    );

    if (!codec.enable()) {

        Serial.println(
            "ERROR: ES8388 initialization failed"
        );

        while (true) {
            delay(1000);
        }
    }

    Serial.println(
        "ES8388 initialized"
    );


    // ------------------------------------------------------------------------
    // I2S
    // ------------------------------------------------------------------------

    /*
     * Optional.
     *
     * Wie beim vorherigen funktionierenden Test:
     *
     * i2s.begin();
     *
     * kann derzeit aktiviert oder weggelassen werden.
     */

    // i2s.begin();


    // ------------------------------------------------------------------------
    // SD_MMC
    // ------------------------------------------------------------------------

    Serial.println(
        "Initializing SD_MMC..."
    );

    /*
     * ESP32 Audio Kit V2.2
     *
     * 1-bit mode.
     */
    if (!SD_MMC.begin(
            "/sdcard",
            false))
    {
        Serial.println(
            "ERROR: SD_MMC initialization failed"
        );

        while (true) {
            delay(1000);
        }
    }

    Serial.println(
        "SD_MMC initialized"
    );


    // ------------------------------------------------------------------------
    // Open MP3
    // ------------------------------------------------------------------------

    Serial.println(
        "Opening /test.mp3..."
    );

    if (!source.open(
            SD_MMC,
            "/test.mp3"))
    {
        Serial.println(
            "ERROR: Could not open /test.mp3"
        );

        while (true) {
            delay(1000);
        }
    }


    Serial.printf(
        "MP3 source opened, size=%llu bytes\n",
        (unsigned long long)source.size()
    );


    // ------------------------------------------------------------------------
    // Start playback
    // ------------------------------------------------------------------------

    Serial.println(
        "Starting MP3 playback..."
    );

    if (!mp3.play(source)) {

        Serial.println(
            "ERROR: MP3 playback could not be started"
        );

        while (true) {
            delay(1000);
        }
    }

    Serial.println(
        "MP3 playback started"
    );
}


// ============================================================================
// Loop
// ============================================================================

void loop()
{
    static uint32_t lastStatus = 0;


    if (millis() - lastStatus >= 1000) {

        lastStatus = millis();


        Serial.print(
            "Playing: "
        );

        Serial.println(
            mp3.isPlaying()
        );
    }
}
