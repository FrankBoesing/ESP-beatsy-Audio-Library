#include <Arduino.h>
#include "audio_usb_manager.h"

#if SOC_USB_OTG_SUPPORTED && CONFIG_TINYUSB_AUDIO_ENABLED

#include "../output_usb.h"
#include "../input_usb.h"
#include <esp32-hal-tinyusb.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <new>
#include <stdarg.h>
#include <stdio.h>

// Redirect native ESP-IDF log output to the managed CDC endpoint.
// The callback must be reentrant: ESP-IDF can call it from multiple tasks.
namespace {
vprintf_like_t previousLogOutput = nullptr;

int managedUsbLogOutput(const char *format, va_list args) {
    USBCDC &cdc = audio_detail::USBManager::instance().serial();

    if ((bool)cdc) {
        char buffer[384];
        va_list copy;
        va_copy(copy, args);
        const int formatted = vsnprintf(buffer, sizeof(buffer), format, copy);
        va_end(copy);

        if (formatted <= 0) {
            return formatted;
        }

        size_t bytes = (size_t)formatted;
        if (bytes >= sizeof(buffer)) {
            // Preserve a line boundary when an unusually long message is truncated.
            buffer[sizeof(buffer) - 2U] = '\n';
            buffer[sizeof(buffer) - 1U] = '\0';
            bytes = sizeof(buffer) - 1U;
        }

        return (int)cdc.write((const uint8_t *)buffer, bytes);
    }

    // Keep the core's original destination (normally UART0) until the USB
    // CDC terminal is connected, and when it is disconnected later.
    vprintf_like_t fallback = previousLogOutput;
    return fallback != nullptr ? fallback(format, args) : 0;
}
} // namespace

namespace audio_detail {

USBManager::USBManager()
#if ARDUINO_USB_CDC_ON_BOOT && !ARDUINO_USB_MODE
    : audioCard_(nullptr),
#else
    : usbSerial_(0),
      audioCard_(nullptr),
#endif
      output_(nullptr),
      input_(nullptr),
      started_(false),
      failed_(false),
      cdcEventsRegistered_(false),
      cdcResetSequenceState_(0),
      cdcResetSequenceStartUs_(0) {}

USBManager &USBManager::instance() {
    static USBManager manager;
    return manager;
}

void USBManager::registerOutput(AudioOutputUSB *output) {
    if (output_ == nullptr || output_ == output) {
        output_ = output;
    } else {
        // The underlying Arduino USBAudioCard owns one microphone endpoint.
        failed_ = true;
    }
}

void USBManager::registerInput(AudioInputUSB *input) {
    if (input_ == nullptr || input_ == input) {
        input_ = input;
    } else {
        // The underlying Arduino USBAudioCard owns one speaker-data callback.
        failed_ = true;
    }
}

bool USBManager::begin() {
    if (started_) {
        return true;
    }
    if (failed_ || (output_ == nullptr && input_ == nullptr)) {
        return false;
    }

    const UAC_SPK_Channels speakerChannels = input_ != nullptr ? UAC_SPK_STEREO : UAC_SPK_NONE;
    const UAC_MIC_Channels microphoneChannels = output_ != nullptr ? UAC_MIC_STEREO : UAC_MIC_NONE;

    // Construct the descriptor-owning object only after every global audio
    // node has registered its endpoint requirements, but before USB.begin().
    audioCard_ = new (audioCardStorage_) USBAudioCard(
        (uint32_t)AUDIO_SAMPLE_RATE_EXACT, UAC_BPS_16,
        speakerChannels, microphoneChannels);

    if (output_ != nullptr) {
        output_->attachManagedDevice(audioCard_);
    }
    if (input_ != nullptr) {
        input_->attachManagedDevice(audioCard_);
    }

#if !ARDUINO_USB_CDC_ON_BOOT || ARDUINO_USB_MODE
    // In this configuration the manager owns the TinyUSB CDC interface.
    // For native CDC-on-boot, the Arduino core initializes USBSerial itself.
    usbSerial_.begin(115200);
#endif

    audioCard_->onEvent(ARDUINO_USB_AUDIO_CARD_INTERFACE_ENABLE_EVENT,
                        &USBManager::onAudioEvent);

    // Handle both the standard CDC reset protocols and the simple RTS pulse
    // used by serial monitors such as ESP Decoder. Disable the core handler so
    // all line-state and line-coding events reach this manager; the manager
    // implements the core's 1200-baud and DTR/RTS bootloader reset sequences.
    if (!cdcEventsRegistered_) {
        serial().enableReboot(false);
        serial().onEvent(ARDUINO_USB_CDC_LINE_STATE_EVENT, &USBManager::onCdcEvent);
        serial().onEvent(ARDUINO_USB_CDC_LINE_CODING_EVENT, &USBManager::onCdcEvent);
        cdcEventsRegistered_ = true;
    }

    if (!audioCard_->begin() || !USB.begin()) {
        failed_ = true;
        return false;
    }

    // ESP-IDF's setDebugOutput() helper only works for CDC interface 0.
    // In a composite Audio+CDC device, CDC may have a nonzero interface index.
    // Explicitly install the ESP-IDF logger callback and write to the same CDC
    // instance exposed as AudioUSBSerial()/Serial instead.
    previousLogOutput = esp_log_set_vprintf(&managedUsbLogOutput);

    started_ = true;
    return true;
}

void USBManager::onCdcEvent(void *arg, esp_event_base_t eventBase,
                            int32_t eventId, void *eventData) {
    (void)arg;
    (void)eventBase;

    if (eventData == nullptr) {
        return;
    }

    USBManager &manager = USBManager::instance();
    const arduino_usb_cdc_event_data_t *event =
        (const arduino_usb_cdc_event_data_t *)eventData;

    if (eventId == ARDUINO_USB_CDC_LINE_CODING_EVENT) {
        // The standard 1200-baud touch requests a reboot into the ROM USB
        // download mode. Preserve the Arduino core's behaviour for this host
        // protocol, including the S3 transition from TinyUSB to USB-Serial/JTAG.
        if (event->line_coding.bit_rate == 1200) {
#if CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3
            usb_persist_restart(RESTART_BOOTLOADER);
#else
            esp_restart();
#endif
        }
        return;
    }

    if (eventId != ARDUINO_USB_CDC_LINE_STATE_EVENT) {
        return;
    }

    const bool dtr = event->line_state.dtr;
    const bool rts = event->line_state.rts;
    const int64_t nowUs = esp_timer_get_time();

    // State 1: DTR low / RTS high. This is the first step of the standard
    // four-state bootloader reset, and also the first half of a legacy/simple
    // RTS pulse. Limit the whole sequence to a short window.
    constexpr int64_t kSequenceTimeoutUs = 2000000;
    constexpr int64_t kSimpleResetMinPulseUs = 30000;
    constexpr int64_t kSimpleResetMaxPulseUs = 1500000;

    uint8_t &state = manager.cdcResetSequenceState_;
    int64_t &sequenceStartUs = manager.cdcResetSequenceStartUs_;

    if (state != 0 && nowUs - sequenceStartUs > kSequenceTimeoutUs) {
        state = 0;
        sequenceStartUs = 0;
    }

    switch (state) {
    case 0:
        if (!dtr && rts) {
            state = 1;
            sequenceStartUs = nowUs;
        }
        break;

    case 1:
        if (dtr && rts) {
            // Standard host sequence: DTR/RTS = 1/1.
            state = 2;
        } else if (!dtr && !rts) {
            // Simple host reset: DTR stays low while RTS is pulsed high then low.
            const int64_t pulseUs = nowUs - sequenceStartUs;
            state = 0;
            sequenceStartUs = 0;
            if (pulseUs >= kSimpleResetMinPulseUs &&
                pulseUs <= kSimpleResetMaxPulseUs) {
                esp_restart();
            }
        } else {
            state = 0;
            sequenceStartUs = 0;
        }
        break;

    case 2:
        if (dtr && !rts) {
            // Standard host sequence: DTR/RTS = 1/0.
            state = 3;
        } else {
            state = 0;
            sequenceStartUs = 0;
            if (!dtr && rts) {
                state = 1;
                sequenceStartUs = nowUs;
            }
        }
        break;

    case 3:
        if (!dtr && !rts) {
            // Standard four-state sequence requests ROM USB download mode.
            state = 0;
            sequenceStartUs = 0;
#if CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3
            usb_persist_restart(RESTART_BOOTLOADER);
#else
            esp_restart();
#endif
        } else {
            state = 0;
            sequenceStartUs = 0;
            if (!dtr && rts) {
                state = 1;
                sequenceStartUs = nowUs;
            }
        }
        break;

    default:
        state = 0;
        sequenceStartUs = 0;
        break;
    }
}

void USBManager::onAudioEvent(void *arg, esp_event_base_t eventBase,
                              int32_t eventId, void *eventData) {
    // USBAudioCard::onEvent() supplies its own event-handler context in
    // arg, so resolve the manager through the singleton instead.
    (void)arg;
    (void)eventBase;
    if (eventData == nullptr ||
        eventId != ARDUINO_USB_AUDIO_CARD_INTERFACE_ENABLE_EVENT) {
        return;
    }

    USBManager *manager = &USBManager::instance();
    const arduino_usb_audio_card_event_data_t *event =
        (const arduino_usb_audio_card_event_data_t *)eventData;

    if (event->interface_enable.interface == UAC_INTERFACE_MIC &&
        manager->output_ != nullptr) {
        manager->output_->setStreaming(event->interface_enable.enable);
    } else if (event->interface_enable.interface == UAC_INTERFACE_SPK &&
               manager->input_ != nullptr) {
        manager->input_->setStreaming(event->interface_enable.enable);
    }
}

} // namespace audio_detail

USBCDC &AudioUSBSerial() {
    return audio_detail::USBManager::instance().serial();
}

#endif // SOC_USB_OTG_SUPPORTED && CONFIG_TINYUSB_AUDIO_ENABLED
