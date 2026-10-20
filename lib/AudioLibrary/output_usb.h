#pragma once

#include <Arduino.h>
#include <atomic>
#include <stdint.h>
#include "AudioStream.h"

#include <soc/soc_caps.h>
#if SOC_USB_OTG_SUPPORTED
#include "sdkconfig.h"
#if CONFIG_TINYUSB_AUDIO_ENABLED
#include <USBCDC.h>

class USBAudioCard;
namespace audio_detail {
class USBManager;
}

/**
 * Stereo audio graph sink that sends 16-bit PCM to an Arduino-ESP32
 * USBAudioCard microphone endpoint configured with UAC_MIC_STEREO.
 *
 * Inputs 0 and 1 carry left and right audio blocks. The samples are interleaved
 * as L, R, L, R, ... for the USB endpoint.
 *
 * With the default constructor, the library manages USB audio, CDC and stream
 * enable events. The explicit USBAudioCard constructor leaves USB startup and
 * event routing to the application.
 *
 * Each AudioOutputUSB::update() interleaves one complete stereo block in a
 * fixed working buffer and copies it into TinyUSB's internal FIFO. The FIFO
 * buffers data between AudioTask updates and asynchronous USB transfers.
 * No loop() polling or additional service task is required; FIFO writes execute
 * in AudioTask context, never in an ISR.
 */
class AudioOutputUSB : public AudioStream {
public:
    // Managed mode: the library configures TinyUSB audio and USB CDC.
    AudioOutputUSB();

    // Advanced mode: the application owns USBAudioCard and its startup.
    explicit AudioOutputUSB(USBAudioCard &usbAudio);
    ~AudioOutputUSB() override;

    AudioOutputUSB(const AudioOutputUSB &) = delete;
    AudioOutputUSB &operator=(const AudioOutputUSB &) = delete;

    // True when USBAudioCard is configured for a stereo microphone endpoint.
    bool isConfigured() const { return configured_; }

    // Called from the USBAudioCard interface-enable event callback.
    void setStreaming(bool enabled);


protected:
    void update() override;
    bool beginHardware() override;

private:
    friend class audio_detail::USBManager;
    void attachManagedDevice(USBAudioCard *usbAudio);

    audio_block_t *inputQueueArray_[2];
    USBAudioCard *usbAudio_;
    bool configured_;
    const bool managerManaged_;

    // One interleaved stereo AudioStream block (512 bytes at 128 samples/channel).
    // USBAudioCard::write() copies it to TinyUSB's FIFO before returning.
    int16_t interleaved_[AUDIO_BLOCK_SAMPLES * 2];

    // Shared with the USB interface callback, which may run outside AudioTask.
    std::atomic<bool> streaming_;
};

// Returns the USB CDC serial interface created by the managed USB device.
// Use it instead of Serial when ARDUINO_USB_CDC_ON_BOOT=0.
USBCDC &AudioUSBSerial();

#endif // CONFIG_TINYUSB_AUDIO_ENABLED
#endif // SOC_USB_OTG_SUPPORTED
