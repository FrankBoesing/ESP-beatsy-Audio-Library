#include "usb_audio.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#if defined(ARDUINO_ARCH_ESP32) && __has_include(<USBAudioCard.h>)
#include <soc/soc_caps.h>
#include <sdkconfig.h>
#if defined(SOC_USB_OTG_SUPPORTED) && SOC_USB_OTG_SUPPORTED && defined(CONFIG_TINYUSB_AUDIO_ENABLED) && CONFIG_TINYUSB_AUDIO_ENABLED
#include <USB.h>
#include <USBAudioCard.h>
#define AUDIO_USB_HAS_ARDUINO_UAC 1
#endif
#endif

#ifndef AUDIO_USB_HAS_ARDUINO_UAC
#define AUDIO_USB_HAS_ARDUINO_UAC 0
#endif

namespace {

constexpr uint32_t USB_AUDIO_RING_FRAMES = 1024;
constexpr uint8_t USB_AUDIO_TX_TASK_PRIORITY = 4;
constexpr uint32_t USB_AUDIO_TX_TASK_STACK = 4096;
constexpr size_t USB_AUDIO_TX_CHUNK_FRAMES = 96;

static_assert((USB_AUDIO_RING_FRAMES & (USB_AUDIO_RING_FRAMES - 1)) == 0,
              "USB audio ring capacity must be a power of two");

class SpscPcmRing {
  public:
    SpscPcmRing() = default;
    ~SpscPcmRing() { delete[] buffer_; }

    SpscPcmRing(const SpscPcmRing &) = delete;
    SpscPcmRing &operator=(const SpscPcmRing &) = delete;

    bool initialize(uint8_t channels) {
        if (channels == 0 || channels > AUDIO_USB_MAX_CHANNELS) {
            return false;
        }
        if (buffer_ != nullptr) {
            return channels_ == channels;
        }

        int16_t *buffer = new (std::nothrow) int16_t[USB_AUDIO_RING_FRAMES * channels];
        if (buffer == nullptr) {
            return false;
        }

        buffer_ = buffer;
        channels_ = channels;
        readIndex_.store(0, std::memory_order_relaxed);
        writeIndex_.store(0, std::memory_order_relaxed);
        return true;
    }

    // Single-producer operation. Reject a whole write rather than publishing
    // a partial audio block when the ring is full.
    bool writeFrames(const int16_t *source, size_t frames) {
        if (frames == 0) {
            return true;
        }
        if (buffer_ == nullptr || source == nullptr || frames > USB_AUDIO_RING_FRAMES) {
            return false;
        }

        const uint32_t write = writeIndex_.load(std::memory_order_relaxed);
        const uint32_t read = readIndex_.load(std::memory_order_acquire);
        const uint32_t used = write - read;
        if (used > USB_AUDIO_RING_FRAMES || frames > USB_AUDIO_RING_FRAMES - used) {
            return false;
        }

        for (size_t frame = 0; frame < frames; ++frame) {
            const size_t ringFrame = (write + frame) & (USB_AUDIO_RING_FRAMES - 1);
            const size_t ringOffset = ringFrame * channels_;
            const size_t sourceOffset = frame * channels_;
            for (uint8_t channel = 0; channel < channels_; ++channel) {
                buffer_[ringOffset + channel] = source[sourceOffset + channel];
            }
        }

        writeIndex_.store(write + static_cast<uint32_t>(frames), std::memory_order_release);
        return true;
    }

    // Single-consumer operation. Missing frames are left to the caller to fill
    // with silence; only available frames are removed from the ring.
    size_t readFrames(int16_t *destination, size_t frames) {
        const size_t count = copyFrames(destination, frames);
        consumeFrames(count);
        return count;
    }

    // Peeking is used for USB IN: keep the samples in the ring until TinyUSB
    // confirms how many bytes it accepted.
    size_t peekFrames(int16_t *destination, size_t frames) const {
        return copyFrames(destination, frames);
    }

    void consumeFrames(size_t frames) {
        const uint32_t read = readIndex_.load(std::memory_order_relaxed);
        const uint32_t write = writeIndex_.load(std::memory_order_acquire);
        const uint32_t available = write - read;
        const uint32_t count = static_cast<uint32_t>(std::min<size_t>(frames, available));
        readIndex_.store(read + count, std::memory_order_release);
    }

    uint32_t availableFrames() const {
        const uint32_t write = writeIndex_.load(std::memory_order_acquire);
        const uint32_t read = readIndex_.load(std::memory_order_relaxed);
        return write - read;
    }

    void discardAll() {
        // Only the consumer may call this method.
        readIndex_.store(writeIndex_.load(std::memory_order_acquire), std::memory_order_release);
    }

  private:
    size_t copyFrames(int16_t *destination, size_t frames) const {
        if (buffer_ == nullptr || destination == nullptr || frames == 0) {
            return 0;
        }

        const uint32_t write = writeIndex_.load(std::memory_order_acquire);
        const uint32_t read = readIndex_.load(std::memory_order_relaxed);
        const uint32_t available = write - read;
        const size_t count = std::min<size_t>(frames, available);

        for (size_t frame = 0; frame < count; ++frame) {
            const size_t ringFrame = (read + frame) & (USB_AUDIO_RING_FRAMES - 1);
            const size_t ringOffset = ringFrame * channels_;
            const size_t destinationOffset = frame * channels_;
            for (uint8_t channel = 0; channel < channels_; ++channel) {
                destination[destinationOffset + channel] = buffer_[ringOffset + channel];
            }
        }
        return count;
    }

    int16_t *buffer_ = nullptr;
    uint8_t channels_ = 0;
    alignas(4) std::atomic<uint32_t> readIndex_{0};
    alignas(4) std::atomic<uint32_t> writeIndex_{0};
};

class AudioUSBBackend {
  public:
    static AudioUSBBackend &instance() {
        static AudioUSBBackend backend;
        return backend;
    }

    bool attachInput(AudioInputUSB *input, uint8_t channels) {
        if (input == nullptr || channels == 0 || channels > AUDIO_USB_MAX_CHANNELS ||
            started_.load(std::memory_order_acquire) || starting_.load(std::memory_order_acquire)) {
            return false;
        }
        if (input_ != nullptr && input_ != input) {
            return false; // The current Arduino USBAudioCard transport has one playback interface.
        }
        input_ = input;
        inputChannels_ = channels;
        inputAttached_.store(true, std::memory_order_release);
        return true;
    }

    void detachInput(AudioInputUSB *input) {
        if (input_ == input) {
            inputNodeEnabled_.store(false, std::memory_order_release);
            inputAttached_.store(false, std::memory_order_release);
            inputStreaming_.store(false, std::memory_order_release);
            input_ = nullptr;
            // Keep inputChannels_ unchanged: the USB descriptor and ring layout
            // cannot be changed after USB startup.
        }
    }

    void setInputEnabled(AudioInputUSB *input, bool enabled) {
        if (input_ == input) {
            inputNodeEnabled_.store(enabled, std::memory_order_release);
        }
    }

    bool attachOutput(AudioOutputUSB *output, uint8_t channels) {
        if (output == nullptr || channels == 0 || channels > AUDIO_USB_MAX_CHANNELS ||
            started_.load(std::memory_order_acquire) || starting_.load(std::memory_order_acquire)) {
            return false;
        }
        if (output_ != nullptr && output_ != output) {
            return false; // The initial backend exposes one capture interface.
        }
        output_ = output;
        outputChannels_ = channels;
        outputAttached_.store(true, std::memory_order_release);
        return true;
    }

    void detachOutput(AudioOutputUSB *output) {
        if (output_ == output) {
            outputNodeEnabled_.store(false, std::memory_order_release);
            outputAttached_.store(false, std::memory_order_release);
            outputStreaming_.store(false, std::memory_order_release);
            output_ = nullptr;
            // Preserve the configured channel count until the USB device is reset.
        }
    }

    void setOutputEnabled(AudioOutputUSB *output, bool enabled) {
        if (output_ == output) {
            outputNodeEnabled_.store(enabled, std::memory_order_release);
        }
    }

    bool start(float sampleRate) {
        if (started_.load(std::memory_order_acquire)) {
            return true;
        }

        bool expected = false;
        if (!starting_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return started_.load(std::memory_order_acquire);
        }

        auto fail = [this]() {
            starting_.store(false, std::memory_order_release);
            return false;
        };

        if ((!inputAttached_.load(std::memory_order_acquire) && !outputAttached_.load(std::memory_order_acquire)) ||
            !std::isfinite(sampleRate) || sampleRate < 8000.0f) {
            return fail();
        }

        // Arduino's current USBAudioCard descriptor implementation exposes at
        // most mono/stereo; the API and rings reserve room for future backends.
        if (inputChannels_ > 2 || outputChannels_ > 2) {
            return fail();
        }

        if (inputChannels_ > 0 && !hostToDevice_.initialize(inputChannels_)) {
            return fail();
        }
        if (outputChannels_ > 0 && !deviceToHost_.initialize(outputChannels_)) {
            return fail();
        }

#if AUDIO_USB_HAS_ARDUINO_UAC
        if (card_ == nullptr) {
            const UAC_SPK_Channels speakerChannels =
                inputChannels_ == 0 ? UAC_SPK_NONE : (inputChannels_ == 1 ? UAC_SPK_MONO : UAC_SPK_STEREO);
            const UAC_MIC_Channels microphoneChannels =
                outputChannels_ == 0 ? UAC_MIC_NONE : (outputChannels_ == 1 ? UAC_MIC_MONO : UAC_MIC_STEREO);

            const uint32_t rate = static_cast<uint32_t>(std::lround(sampleRate));
            card_ = new (std::nothrow) USBAudioCard(rate, UAC_BPS_16, speakerChannels, microphoneChannels);
            if (card_ == nullptr) {
                return fail();
            }
            configuredSampleRate_ = rate;
            card_->onData(&AudioUSBBackend::onHostPlaybackData);
            card_->onEvent(ARDUINO_USB_AUDIO_CARD_INTERFACE_ENABLE_EVENT, &AudioUSBBackend::onUsbAudioEvent);
        } else if (configuredSampleRate_ != static_cast<uint32_t>(std::lround(sampleRate))) {
            return fail();
        }

        if (!card_->begin()) {
            return fail();
        }

        if (outputChannels_ > 0 && txTask_ == nullptr) {
            const BaseType_t taskCreated =
                xTaskCreate(&AudioUSBBackend::txTaskEntry, "AudioUSBTx", USB_AUDIO_TX_TASK_STACK, this,
                            USB_AUDIO_TX_TASK_PRIORITY, &txTask_);
            if (taskCreated != pdPASS || txTask_ == nullptr) {
                card_->end();
                txTask_ = nullptr;
                return fail();
            }
        }

        // Register the audio interface before starting TinyUSB. With
        // ARDUINO_USB_MODE=0 and ARDUINO_USB_CDC_ON_BOOT=1, the core can add
        // CDC alongside this audio interface to the same USB configuration.
        // That CDC+UAC combination must still be verified on the target board.
        USB.begin();

        started_.store(true, std::memory_order_release);
        starting_.store(false, std::memory_order_release);
        return true;
#else
        // Keep the AudioStream API buildable on targets/cores without the
        // Arduino USBAudioCard class. begin() reports failure on those builds.
        return fail();
#endif
    }

    bool isStarted() const { return started_.load(std::memory_order_acquire); }

    bool canChangeSampleRate() const {
#if AUDIO_USB_HAS_ARDUINO_UAC
        return !started_.load(std::memory_order_acquire) && card_ == nullptr;
#else
        return !started_.load(std::memory_order_acquire);
#endif
    }

    bool inputStreaming() const {
        return inputStreaming_.load(std::memory_order_acquire);
    }

    bool outputStreaming() const {
        return outputStreaming_.load(std::memory_order_acquire);
    }

    size_t readInput(int16_t *interleaved, size_t frames) {
        return hostToDevice_.readFrames(interleaved, frames);
    }

    void discardInput() { hostToDevice_.discardAll(); }

    bool writeOutput(const int16_t *interleaved, size_t frames) {
        return deviceToHost_.writeFrames(interleaved, frames);
    }

  private:
    static void onHostPlaybackData(void *data, uint16_t len) {
        AudioUSBBackend &backend = instance();
        if (!backend.inputAttached_.load(std::memory_order_acquire) ||
            !backend.inputNodeEnabled_.load(std::memory_order_acquire) ||
            !backend.inputStreaming_.load(std::memory_order_acquire) ||
            backend.inputChannels_ == 0 || data == nullptr) {
            return;
        }

        const size_t frameBytes = static_cast<size_t>(backend.inputChannels_) * sizeof(int16_t);
        const size_t frames = len / frameBytes;
        if (frames == 0) {
            return;
        }

        // USBAudioCard supplies interleaved signed 16-bit PCM for UAC_BPS_16.
        (void)backend.hostToDevice_.writeFrames(static_cast<const int16_t *>(data), frames);
    }

    static void onUsbAudioEvent(void *, esp_event_base_t, int32_t eventId, void *eventData) {
        if (eventId != ARDUINO_USB_AUDIO_CARD_INTERFACE_ENABLE_EVENT || eventData == nullptr) {
            return;
        }

        const auto *event = static_cast<const arduino_usb_audio_card_event_data_t *>(eventData);
        AudioUSBBackend &backend = instance();

        if (event->interface_enable.interface == UAC_INTERFACE_SPK) {
            backend.inputStreaming_.store(event->interface_enable.enable, std::memory_order_release);
        } else if (event->interface_enable.interface == UAC_INTERFACE_MIC) {
            backend.outputStreaming_.store(event->interface_enable.enable, std::memory_order_release);
        }
    }

    static void txTaskEntry(void *arg) {
        static_cast<AudioUSBBackend *>(arg)->txTaskLoop();
    }

    void txTaskLoop() {
#if AUDIO_USB_HAS_ARDUINO_UAC
        alignas(4) int16_t chunk[USB_AUDIO_TX_CHUNK_FRAMES * AUDIO_USB_MAX_CHANNELS];

        for (;;) {
            if (!started_.load(std::memory_order_acquire) ||
                !outputAttached_.load(std::memory_order_acquire) ||
                !outputNodeEnabled_.load(std::memory_order_acquire) ||
                !outputStreaming_.load(std::memory_order_acquire) ||
                card_ == nullptr || outputChannels_ == 0) {
                deviceToHost_.discardAll();
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }

            const size_t available = deviceToHost_.availableFrames();
            if (available == 0) {
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }

            const size_t wanted = std::min<size_t>(available, USB_AUDIO_TX_CHUNK_FRAMES);
            const size_t copied = deviceToHost_.peekFrames(chunk, wanted);
            if (copied == 0) {
                vTaskDelay(1);
                continue;
            }

            const size_t frameBytes = static_cast<size_t>(outputChannels_) * sizeof(int16_t);
            const size_t requestBytes = copied * frameBytes;
            const uint16_t accepted = card_->write(chunk, static_cast<uint16_t>(requestBytes));
            const size_t acceptedFrames = std::min<size_t>(copied, accepted / frameBytes);

            if (acceptedFrames > 0) {
                deviceToHost_.consumeFrames(acceptedFrames);
            } else {
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        }
#endif
        vTaskDelete(nullptr);
    }

    AudioInputUSB *input_ = nullptr;
    AudioOutputUSB *output_ = nullptr;
    uint8_t inputChannels_ = 0;
    uint8_t outputChannels_ = 0;

    SpscPcmRing hostToDevice_;
    SpscPcmRing deviceToHost_;

    std::atomic<bool> inputAttached_{false};
    std::atomic<bool> outputAttached_{false};
    std::atomic<bool> inputNodeEnabled_{false};
    std::atomic<bool> outputNodeEnabled_{false};
    std::atomic<bool> inputStreaming_{false};
    std::atomic<bool> outputStreaming_{false};
    std::atomic<bool> starting_{false};
    std::atomic<bool> started_{false};

#if AUDIO_USB_HAS_ARDUINO_UAC
    USBAudioCard *card_ = nullptr;
    uint32_t configuredSampleRate_ = 0;
    TaskHandle_t txTask_ = nullptr;
#endif
};

} // namespace

namespace audio_usb_detail {

uint8_t normalizeChannelCount(uint8_t channels) {
    return (channels >= 1 && channels <= AUDIO_USB_MAX_CHANNELS) ? channels : 0;
}

bool attachInput(AudioInputUSB *input, uint8_t channels) {
    return AudioUSBBackend::instance().attachInput(input, channels);
}

void detachInput(AudioInputUSB *input) {
    AudioUSBBackend::instance().detachInput(input);
}

void setInputEnabled(AudioInputUSB *input, bool enabled) {
    AudioUSBBackend::instance().setInputEnabled(input, enabled);
}

bool attachOutput(AudioOutputUSB *output, uint8_t channels) {
    return AudioUSBBackend::instance().attachOutput(output, channels);
}

void detachOutput(AudioOutputUSB *output) {
    AudioUSBBackend::instance().detachOutput(output);
}

void setOutputEnabled(AudioOutputUSB *output, bool enabled) {
    AudioUSBBackend::instance().setOutputEnabled(output, enabled);
}

bool start(float sampleRate) {
    return AudioUSBBackend::instance().start(sampleRate);
}

bool isStarted() {
    return AudioUSBBackend::instance().isStarted();
}

bool canChangeSampleRate() {
    return AudioUSBBackend::instance().canChangeSampleRate();
}

bool inputStreaming() {
    return AudioUSBBackend::instance().inputStreaming();
}

bool outputStreaming() {
    return AudioUSBBackend::instance().outputStreaming();
}

size_t readInput(int16_t *interleaved, size_t frames) {
    return AudioUSBBackend::instance().readInput(interleaved, frames);
}

void discardInput() {
    AudioUSBBackend::instance().discardInput();
}

bool writeOutput(const int16_t *interleaved, size_t frames) {
    return AudioUSBBackend::instance().writeOutput(interleaved, frames);
}

} // namespace audio_usb_detail

AudioInputUSB::AudioInputUSB(uint8_t channels)
    : AudioStream(0, nullptr), channels_(normalize(channels)) {
    registered_ = audio_usb_detail::attachInput(this, channels_);
}

AudioInputUSB::~AudioInputUSB() {
    enabled_.store(false, std::memory_order_release);
    audio_usb_detail::setInputEnabled(this, false);
    audio_usb_detail::detachInput(this);
}

bool AudioInputUSB::begin() {
    if (!registered_ || !audio_usb_detail::start(AudioStream::sampleRate())) {
        return false;
    }
    audio_usb_detail::setInputEnabled(this, true);
    enabled_.store(true, std::memory_order_release);
    return true;
}

void AudioInputUSB::end() {
    enabled_.store(false, std::memory_order_release);
    audio_usb_detail::setInputEnabled(this, false);
}

bool AudioInputUSB::isRunning() const {
    return enabled_.load(std::memory_order_acquire) && audio_usb_detail::isStarted();
}

bool AudioInputUSB::isStreaming() const {
    return isRunning() && audio_usb_detail::inputStreaming();
}

bool AudioInputUSB::setSampleRate(float sampleRate) {
    if (!audio_usb_detail::canChangeSampleRate()) {
        return false;
    }
    return AudioStream::setSampleRate(sampleRate);
}

bool AudioInputUSB::beginHardware() {
    return begin();
}

OSPEED
void AudioInputUSB::update() {
    if (!registered_ || channels_ == 0) {
        return;
    }

    audio_block_t *blocks[AUDIO_USB_MAX_CHANNELS] = {};
    for (uint8_t channel = 0; channel < channels_; ++channel) {
        blocks[channel] = allocate();
    }

    alignas(4) int16_t interleaved[AUDIO_BLOCK_SAMPLES * AUDIO_USB_MAX_CHANNELS] = {};
    if (enabled_.load(std::memory_order_acquire) && audio_usb_detail::inputStreaming()) {
        (void)audio_usb_detail::readInput(interleaved, AUDIO_BLOCK_SAMPLES);
    } else {
        audio_usb_detail::discardInput();
    }

    for (uint8_t channel = 0; channel < channels_; ++channel) {
        audio_block_t *block = blocks[channel];
        if (block == nullptr) {
            continue;
        }

        for (size_t frame = 0; frame < AUDIO_BLOCK_SAMPLES; ++frame) {
            block->data[frame] = interleaved[frame * channels_ + channel];
        }

        transmit(block, channel);
        release(block);
    }
}

AudioOutputUSB::AudioOutputUSB(uint8_t channels)
    : AudioStream(audio_usb_detail::normalizeChannelCount(channels), inputQueueArray),
      channels_(audio_usb_detail::normalizeChannelCount(channels)) {
    registered_ = audio_usb_detail::attachOutput(this, channels_);
}

AudioOutputUSB::~AudioOutputUSB() {
    enabled_.store(false, std::memory_order_release);
    audio_usb_detail::setOutputEnabled(this, false);
    audio_usb_detail::detachOutput(this);
}

bool AudioOutputUSB::begin() {
    if (!registered_ || !audio_usb_detail::start(AudioStream::sampleRate())) {
        return false;
    }
    audio_usb_detail::setOutputEnabled(this, true);
    enabled_.store(true, std::memory_order_release);
    return true;
}

void AudioOutputUSB::end() {
    enabled_.store(false, std::memory_order_release);
    audio_usb_detail::setOutputEnabled(this, false);
}

bool AudioOutputUSB::isRunning() const {
    return enabled_.load(std::memory_order_acquire) && audio_usb_detail::isStarted();
}

bool AudioOutputUSB::isStreaming() const {
    return isRunning() && audio_usb_detail::outputStreaming();
}

bool AudioOutputUSB::setSampleRate(float sampleRate) {
    if (!audio_usb_detail::canChangeSampleRate()) {
        return false;
    }
    return AudioStream::setSampleRate(sampleRate);
}

bool AudioOutputUSB::beginHardware() {
    return begin();
}

OSPEED
void AudioOutputUSB::update() {
    if (channels_ == 0) {
        return;
    }

    audio_block_t *blocks[AUDIO_USB_MAX_CHANNELS] = {};
    for (uint8_t channel = 0; channel < channels_; ++channel) {
        blocks[channel] = receiveReadOnly(channel);
    }

    if (registered_ && enabled_.load(std::memory_order_acquire) && audio_usb_detail::outputStreaming()) {
        alignas(4) int16_t interleaved[AUDIO_BLOCK_SAMPLES * AUDIO_USB_MAX_CHANNELS] = {};

        for (size_t frame = 0; frame < AUDIO_BLOCK_SAMPLES; ++frame) {
            for (uint8_t channel = 0; channel < channels_; ++channel) {
                interleaved[frame * channels_ + channel] =
                    blocks[channel] != nullptr ? blocks[channel]->data[frame] : 0;
            }
        }

        // If the ring is full, drop this block rather than blocking the audio
        // scheduler or moving the consumer's read index from another task.
        (void)audio_usb_detail::writeOutput(interleaved, AUDIO_BLOCK_SAMPLES);
    }

    for (uint8_t channel = 0; channel < channels_; ++channel) {
        if (blocks[channel] != nullptr) {
            release(blocks[channel]);
        }
    }
}
