#pragma once

#include <Arduino.h>
#include <atomic>
#include <stdint.h>
#include "AudioStream.h"

#include <soc/soc_caps.h>
#if SOC_USB_OTG_SUPPORTED
#include "sdkconfig.h"
#if CONFIG_TINYUSB_AUDIO_ENABLED
class USBAudioCard;

/**
 * Stereo audio graph sink that sends 16-bit PCM to an Arduino-ESP32
 * USBAudioCard microphone endpoint configured with UAC_MIC_STEREO.
 *
 * Inputs 0 and 1 carry left and right audio blocks. The samples are interleaved
 * as L, R, L, R, ... for the USB endpoint.
 *
 * The host controls whether the USB microphone is streaming. The application
 * should forward ARDUINO_USB_AUDIO_CARD_INTERFACE_ENABLE_EVENT to setStreaming().
 *
 * The ring buffer is drained automatically from AudioOutputUSB::update() at the
 * AudioStream cadence. No loop() polling or additional service task is required.
 * TinyUSB FIFO writes therefore execute in the AudioTask context, never in an ISR.
 */
class AudioOutputUSB : public AudioStream {
public:
    explicit AudioOutputUSB(USBAudioCard &usbAudio);
    ~AudioOutputUSB() override;

    AudioOutputUSB(const AudioOutputUSB &) = delete;
    AudioOutputUSB &operator=(const AudioOutputUSB &) = delete;

    // True when USBAudioCard is configured for a stereo microphone endpoint.
    bool isConfigured() const { return configured_; }

    // Called from the USBAudioCard interface-enable event callback.
    void setStreaming(bool enabled);

    // Set the global AudioStream graph rate before AudioMemory() initializes the scheduler.
    // USBAudioCard's advertised USB rate must be configured to the same value.
    bool setGraphSampleRate(uint32_t sampleRateHz);

    void update() override;

private:
    static constexpr uint32_t kRingSamples = 1024;
    static constexpr uint32_t kRingMask = kRingSamples - 1;

    audio_block_t *inputQueueArray_[2];
    USBAudioCard &usbAudio_;
    const bool configured_;
    int16_t ring_[kRingSamples];

    // Monotonic indices are owned exclusively by update() in AudioTask.
    uint32_t readIndex_;
    uint32_t writeIndex_;

    // Shared with the USB interface callback, which may run outside AudioTask.
    std::atomic<bool> streaming_;
};
#endif // CONFIG_TINYUSB_AUDIO_ENABLED
#endif // SOC_USB_OTG_SUPPORTED
