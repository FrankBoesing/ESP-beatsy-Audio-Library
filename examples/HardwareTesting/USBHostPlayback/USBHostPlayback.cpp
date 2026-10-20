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
#include <USB.h>
#include <USBAudioCard.h>

static const uint32_t usbRateHz = (uint32_t)AUDIO_SAMPLE_RATE_EXACT;

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

USBAudioCard usbAudio(usbRateHz, UAC_BPS_16, UAC_SPK_STEREO, UAC_MIC_NONE);
AudioInputUSB usbInput(usbAudio);

AudioConnection usbLeft(usbInput, 0, i2sOut, 0);
AudioConnection usbRight(usbInput, 1, i2sOut, 1);

static void stopOnError(const char *message) {
    Serial.println(message);
    while (true) {
        delay(1000);
    }
}

static void onUsbAudioEvent(void *arg, esp_event_base_t eventBase,
                            int32_t eventId, void *eventData) {
    (void)arg;
    (void)eventBase;
    (void)eventId;

    if (eventData == nullptr) {
        return;
    }

    const arduino_usb_audio_card_event_data_t *data =
        (const arduino_usb_audio_card_event_data_t *)eventData;

    if (data->interface_enable.interface != UAC_INTERFACE_SPK) {
        return;
    }

    usbInput.setStreaming(data->interface_enable.enable);
    Serial.printf("USB playback %s\n",
                  data->interface_enable.enable ? "started" : "stopped");
}

void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println();
    Serial.println("USB host playback -> I2S audio output");
    Serial.printf("Audio rate: %lu Hz, stereo, 16-bit PCM\n",
                  (unsigned long)usbRateHz);

    if (!usbInput.isConfigured()) {
        stopOnError("ERROR: AudioInputUSB configuration does not match AUDIO_SAMPLE_RATE_EXACT.");
    }

    // Start the audio graph and its I2S clock before the PC can start streaming.
    AudioMemory(12);

#if defined(AUDIO_CODEC_ES8311)
    const bool codecReady = codec.enable(usbRateHz);
#else
    const bool codecReady = codec.enable();
#endif
    if (!codecReady || !codec.volume(0.2f)) {
        stopOnError("ERROR: Audio codec initialization failed.");
    }

    // Register before starting USB so interface-enable events cannot be missed.
    usbAudio.onEvent(ARDUINO_USB_AUDIO_CARD_INTERFACE_ENABLE_EVENT, onUsbAudioEvent);

    if (!usbAudio.begin()) {
        stopOnError("ERROR: Could not start USBAudioCard.");
    }
    if (!USB.begin()) {
        stopOnError("ERROR: USB.begin() failed.");
    }

    Serial.println("Ready. Select the ESP32 USB audio device as the PC's audio output.");
}

void loop() {
    delay(1000);
}
