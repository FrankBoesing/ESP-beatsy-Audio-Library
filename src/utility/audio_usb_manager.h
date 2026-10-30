#pragma once

#include <Arduino.h>
#include <soc/soc_caps.h>

#if SOC_USB_OTG_SUPPORTED
#include "sdkconfig.h"
#endif

#if SOC_USB_OTG_SUPPORTED && CONFIG_TINYUSB_AUDIO_ENABLED

#include <USB.h>
#include <USBCDC.h>
#include <USBAudioCard.h>
#include <stdint.h>

class AudioOutputUSB;
class AudioInputUSB;

namespace audio_detail {

// Owns the single TinyUSB device used by the managed USB audio nodes.
// Audio node constructors register their endpoint requirements before
// AudioMemory() invokes beginHardware() on the graph.
class USBManager {
public:
    static USBManager &instance();

    void registerOutput(AudioOutputUSB *output);
    void registerInput(AudioInputUSB *input);
    bool begin();

    // Reuse the Arduino core's CDC instance when Serial is configured for
    // native USB CDC. Otherwise this manager owns a CDC interface.
    USBCDC &serial() {
#if ARDUINO_USB_CDC_ON_BOOT && !ARDUINO_USB_MODE
        return USBSerial;
#else
        return usbSerial_;
#endif
    }

private:
    USBManager();

    static void onAudioEvent(void *arg, esp_event_base_t eventBase,
                             int32_t eventId, void *eventData);
    static void onCdcEvent(void *arg, esp_event_base_t eventBase,
                           int32_t eventId, void *eventData);

#if !ARDUINO_USB_CDC_ON_BOOT || ARDUINO_USB_MODE
    USBCDC usbSerial_;
#endif
    alignas(USBAudioCard) uint8_t audioCardStorage_[sizeof(USBAudioCard)];
    USBAudioCard *audioCard_;
    AudioOutputUSB *output_;
    AudioInputUSB *input_;
    bool started_;
    bool failed_;
    bool cdcEventsRegistered_;
    uint8_t cdcResetSequenceState_;
    int64_t cdcResetSequenceStartUs_;
};

} // namespace audio_detail

// CDC endpoint used by USB-audio builds. It resolves to the Arduino core's
// USBSerial when ARDUINO_USB_CDC_ON_BOOT=1 and otherwise to the manager's CDC.
USBCDC &AudioUSBSerial();

#endif // SOC_USB_OTG_SUPPORTED && CONFIG_TINYUSB_AUDIO_ENABLED
