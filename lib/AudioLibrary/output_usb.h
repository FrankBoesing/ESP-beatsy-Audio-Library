#pragma once
#ifndef AUDIO_OUTPUT_USB_H
#define AUDIO_OUTPUT_USB_H

#include <Arduino.h>
#include <atomic>
#include <stdint.h>

#include "AudioStream.h"
#include "usb_audio_backend.h"

// With USB CDC auto-start disabled, the USB audio backend registers this CDC
// interface before USB.begin(). Use USBSerial for logs over the same USB cable.
#if defined(ARDUINO_ARCH_ESP32)
#include <soc/soc_caps.h>
#include <sdkconfig.h>
#if SOC_USB_OTG_SUPPORTED && (!defined(ARDUINO_USB_CDC_ON_BOOT) || !ARDUINO_USB_CDC_ON_BOOT) && \
    defined(CONFIG_TINYUSB_CDC_ENABLED) && CONFIG_TINYUSB_CDC_ENABLED
#include <USB.h>
extern USBCDC USBSerial;
#endif
#endif

/**
 * USB audio sink: sends samples from the AudioStream graph to the USB host.
 *
 * It follows the AudioOutputI2S model: input 0 is left and input 1 is right
 * in the default stereo configuration. Higher channel counts are reserved
 * for backends that support multichannel USB Audio descriptors.
 */
class AudioOutputUSB : public AudioStream {
  public:
    explicit AudioOutputUSB(uint8_t channels = 2);
    ~AudioOutputUSB() override;

    bool begin();
    void end();

    bool isRunning() const;
    // True when the Arduino/TinyUSB audio transmit API accepted audio recently.
    bool isStreaming() const;
    uint8_t channelCount() const { return channels_; }

    // Sets the graph-wide rate before USB audio is initialized.
    bool setSampleRate(float sampleRate);

  protected:
    void update() override;
    bool beginHardware() override;

  private:
    uint8_t channels_;
    bool registered_ = false;
    std::atomic<bool> enabled_{false};
    audio_block_t *inputQueueArray[AUDIO_USB_MAX_CHANNELS] = {};

    AudioOutputUSB(const AudioOutputUSB &) = delete;
    AudioOutputUSB &operator=(const AudioOutputUSB &) = delete;
};

#endif // AUDIO_OUTPUT_USB_H
