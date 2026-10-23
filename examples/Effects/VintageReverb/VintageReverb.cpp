/*
 * VERBtrix-derived vintage reverb test for ESP-beatsy.
 * Original DSP core and algorithm by Bas Scheffers, MIT-licensed.
 *
 * Connect I2S line-in to I2S line-out through the stereo reverb.
 */
#include <Audio.h>

AudioInputI2S i2sIn;
AudioOutputI2S i2sOut({
    PIN_I2S_BLCK, // BCLK
    PIN_I2S_WS,   // WS / LRCLK
    PIN_I2S_DOUT, // DOUT
    PIN_I2S_MLCK  // MCLK
});

AudioEffectVintageReverb reverb;
AudioConnection patchInL(i2sIn, 0, reverb, 0);
AudioConnection patchInR(i2sIn, 1, reverb, 1);
AudioConnection patchOutL(reverb, 0, i2sOut, 0);
AudioConnection patchOutR(reverb, 1, i2sOut, 1);

#if defined(AUDIO_CODEC_ES8388)
AudioControlES8388 codec({PIN_I2C_SDA, PIN_I2C_SCL, PIN_AMPLIFIER, PIN_AMPLIFIER_ACTIVE});
#elif defined(AUDIO_CODEC_ES8311)
AudioControlES8311 codec({PIN_I2C_SDA, PIN_I2C_SCL, PIN_AMPLIFIER, PIN_AMPLIFIER_ACTIVE});
#else
#error "AUDIO_CODEC not defined"
#endif

void setup()
{
    Serial.begin(115200);
    delay(500);
    AudioMemory(32);

    codec.enable();
    codec.inputSelect(AUDIO_INPUT_LINEIN);
    codec.inputLevel(0.8f);
    codec.volume(0.2f);

    if (!reverb.begin()) {
        ESP_LOGE("VerbtrixExample", "Reverb initialization failed; effect passes audio through");
        return;
    }

    reverb.preset(AudioEffectVintageReverb::CONCERT_HALL);
    reverb.mix(0.35f);
    Serial.printf("VERBtrix ready; delay storage in %s\n",
                  reverb.usingPSRAM() ? "PSRAM" : "internal RAM");
}

void loop()
{
    static uint32_t lastStatus = 0;
    const uint32_t now = millis();
    if (now - lastStatus >= 1000) {
        lastStatus = now;
        Serial.printf("Audio CPU: %.1f %% | Audio memory blocks: %u\n",
                      AudioProcessorUsage(), AudioMemoryUsage());
    }
}
