/*
 * USB host playback -> ESP32 audio output.
 *
 * The ESP32-S3 appears to the PC as a USB audio playback device (speakers).
 * PCM from the PC is received by AudioInputUSB and passed to the board's
 * I2S codec output.
 *
 * PlatformIO: use env:freenove_esp32_s3_display_USBAUDIO.
 * That environment configures 48 kHz, 16-bit stereo and native TinyUSB.
 */

#if !defined(ARDUINO_USB_MODE) || ARDUINO_USB_MODE != 0 || \
    !defined(ARDUINO_USB_CDC_ON_BOOT) || ARDUINO_USB_CDC_ON_BOOT != 0
#error "USBHostPlayback requires TinyUSB mode and ARDUINO_USB_CDC_ON_BOOT=0."
#endif

#include <Arduino.h>
#include <Audio.h>

AudioOutputI2S i2sOut({
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

AudioInputUSB usbInput;

AudioConnection usbLeft(usbInput, 0, i2sOut, 0);
AudioConnection usbRight(usbInput, 1, i2sOut, 1);

static void stopOnError(const char *message) {
    Serial.println(message);
    while (true) {
        delay(1000);
    }
}

void setup() {
    // AudioOutputI2S initializes before AudioInputUSB because it was declared
    // first; the USB manager starts only after all graph nodes are registered.
    AudioMemory(12);

    Serial.println();
    Serial.println("USB host playback -> I2S audio output");
    Serial.printf("Audio rate: %lu Hz, stereo, 16-bit PCM\n",
                            (unsigned long)(uint32_t)AUDIO_SAMPLE_RATE_EXACT);

    if (!usbInput.isConfigured()) {
        stopOnError("ERROR: managed AudioInputUSB initialization failed.");
    }

#if defined(AUDIO_CODEC_ES8311)
    const bool codecReady = codec.enable((uint32_t)AUDIO_SAMPLE_RATE_EXACT);
#else
    const bool codecReady = codec.enable();
#endif
    if (!codecReady || !codec.volume(0.2f)) {
        stopOnError("ERROR: Audio codec initialization failed.");
    }

    Serial.println("Ready. Select the ESP32 USB audio device as the PC's audio output.");
}

void loop() {
    delay(1000);
}
