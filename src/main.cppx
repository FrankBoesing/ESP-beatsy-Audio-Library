#include <Arduino.h>
#include <SD_MMC.h>

#include "AudioStream.h"
#include "play_sd_wav.h"
#include "output_i2s.h"
#include "control_es8388.h"


AudioPlaySdWav wav;

AudioOutputI2S i2s({
    27,  // BCLK
    25,  // WS / LRCLK
    26,  // DOUT
    0    // MCLK
});

AudioControlES8388 codec;

AudioConnection patchCord1(wav, 0, i2s, 0);
AudioConnection patchCord2(wav, 0, i2s, 1);


void setup()
{
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("================================");
    Serial.println(" AUDIO PLAY SD WAV TEST");
    Serial.println("================================");

    AudioMemory(12);

    Serial.println("Initializing ES8388...");

    if (!codec.enable()) {
        Serial.println("ERROR: ES8388 initialization failed");

        while (1) {
            delay(1000);
        }
    }

    Serial.println("ES8388 initialized");

    // Optional – currently both variants work.
    // i2s.begin();

    Serial.println("Initializing SD_MMC...");

    // ESP32 Audio Kit V2.2:
    // first test in 1-bit mode.
    if (!SD_MMC.begin("/sdcard", true)) {

        Serial.println(
            "ERROR: SD_MMC initialization failed"
        );

        while (1) {
            delay(1000);
        }
    }

    Serial.println("SD_MMC initialized");

    Serial.println("Opening /test.wav...");

    //if (!wav.play("/test.wav")) {
    if (!wav.play(SD_MMC, "/test.wav")) {

        Serial.println(
            "ERROR: WAV playback could not be started"
        );

        while (1) {
            delay(1000);
        }
    }

    Serial.println("WAV playback started");
}


void loop()
{
    static uint32_t lastStatus = 0;

    if (millis() - lastStatus >= 1000) {

        lastStatus = millis();

        Serial.print("Playing: ");
        Serial.print(wav.isPlaying());

        Serial.print("  Position: ");
        Serial.print(wav.positionMillis());

        Serial.print(" ms / ");
        Serial.print(wav.lengthMillis());

        Serial.println(" ms");
    }
}
