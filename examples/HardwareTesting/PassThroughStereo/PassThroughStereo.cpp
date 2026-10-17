/*
 * A simple hardware test which receives audio from the audio shield
 * Line-In pins and sends it to the Line-Out pins and headphone jack.
 */

#include <Audio.h>

AudioOutputI2S i2s2({
    PIN_I2S_BLCK, // BCLK
    PIN_I2S_WS,   // WS / LRCLK
    PIN_I2S_DOUT, // DOUT
    PIN_I2S_MLCK  // MCLK
});

#if defined(AUDIO_CODEC_ES8388)
AudioControlES8388 codec({PIN_I2C_SDA, PIN_I2C_SCL, PIN_AMPLIFIER, PIN_AMPLIFIER_ACTIVE});
#elif defined(AUDIO_CODEC_ES8311)
AudioControlES8311 codec({PIN_I2C_SDA, PIN_I2C_SCL, PIN_AMPLIFIER, PIN_AMPLIFIER_ACTIVE});
#else
#error "AUDIO_CODEC not defined"
#endif

AudioInputI2S i2s1;
AudioConnection patchCord1(i2s1, 0, i2s2, 0);
AudioConnection patchCord2(i2s1, 1, i2s2, 1);

// On some ESP Audio Kit boards, these inputs may need to be swapped.
const int myInput = AUDIO_INPUT_LINEIN;
// const int myInput = AUDIO_INPUT_MIC;

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("Started");

    AudioMemory(12);

    codec.enable();
    codec.inputSelect(myInput);
    codec.inputLevel(0.8f);
    codec.volume(0.2f);
}

void loop() {
    static uint32_t lastStatus = 0;
    const uint32_t now = millis();

    if (now - lastStatus >= 1000) {
        lastStatus = now;

        Serial.printf(
            "RX blocks: %lu | RX errors: %lu | Dropped blocks: %lu\n",
            (unsigned long)i2s1.rxBlockCount(),
            (unsigned long)i2s1.rxErrorCount(),
            (unsigned long)i2s1.droppedBlockCount()
        );
    }
}
