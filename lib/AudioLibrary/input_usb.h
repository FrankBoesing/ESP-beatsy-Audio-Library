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
namespace audio_detail {
class USBManager;
}

/**
 * Stereo audio source receiving 16-bit PCM from the host through the
 * USBAudioCard speaker endpoint.
 *
 * Output 0 is left; output 1 is right. The framework receives USB packets on
 * its own task, so a small two-block SPSC buffer decouples that callback from
 * AudioTask. No additional task or polling in loop() is used.
 *
 * The default constructor lets the library configure USB audio, CDC and stream
 * enable events. The explicit USBAudioCard constructor leaves USB startup and
 * event routing to the application.
 */
class AudioInputUSB : public AudioStream {
public:
    // Managed mode: the library configures TinyUSB audio and USB CDC.
    AudioInputUSB();

    // Advanced mode: the application owns USBAudioCard and its startup.
    explicit AudioInputUSB(USBAudioCard &usbAudio);
    ~AudioInputUSB() override;

    AudioInputUSB(const AudioInputUSB &) = delete;
    AudioInputUSB &operator=(const AudioInputUSB &) = delete;

    bool isConfigured() const { return configured_ && registered_; }

    // Call for the UAC_INTERFACE_SPK interface-enable event.
    void setStreaming(bool enabled);

protected:
    void update() override;
    bool beginHardware() override;

private:
    friend class audio_detail::USBManager;
    void attachManagedDevice(USBAudioCard *usbAudio);
    void registerDataCallback();

    static constexpr uint32_t RX_RING_BLOCKS = 2;
    static_assert((RX_RING_BLOCKS & (RX_RING_BLOCKS - 1U)) == 0,
                  "USB RX ring size must be a power of two");

    static void onUsbData(void *data, uint16_t len);
    void receiveData(void *data, uint16_t len);

    USBAudioCard *usbAudio_;
    bool configured_;
    const bool managerManaged_;
    bool registered_ = false;

    // One ring slot is filled by the USBAudioCard receive task while AudioTask
    // consumes the other; each slot is one interleaved stereo audio block.
    alignas(4) int16_t rxSampleRing_[RX_RING_BLOCKS][AUDIO_BLOCK_SAMPLES * 2] = {};
    std::atomic<uint32_t> writeBlockIndex_{0};
    std::atomic<uint32_t> readBlockIndex_{0};
    uint16_t fillFrames_ = 0;

    // Shared with the USB audio receive task / interface event callback.
    std::atomic<bool> streaming_{false};
    std::atomic<bool> resetPending_{true};
    std::atomic<bool> discardPending_{true};

    // USBAudioCard exposes one speaker-data callback and is itself singleton.
    static std::atomic<AudioInputUSB *> instance_;
    static std::atomic<uint32_t> callbackCount_;
};
#endif // CONFIG_TINYUSB_AUDIO_ENABLED
#endif // SOC_USB_OTG_SUPPORTED
