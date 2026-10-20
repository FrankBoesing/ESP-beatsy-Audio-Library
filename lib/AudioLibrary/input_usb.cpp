#include "input_usb.h"

#if SOC_USB_OTG_SUPPORTED && CONFIG_TINYUSB_AUDIO_ENABLED

#include <USBAudioCard.h>

std::atomic<AudioInputUSB *> AudioInputUSB::instance_{nullptr};
std::atomic<uint32_t> AudioInputUSB::callbackCount_{0};

AudioInputUSB::AudioInputUSB(USBAudioCard &usbAudio)
    : AudioStream(0, nullptr),
      usbAudio_(usbAudio),
      configured_(usbAudio.speakerChannels() == UAC_SPK_STEREO &&
                  usbAudio.bitsPerSample() == UAC_BPS_16 &&
                  usbAudio.sampleRate() == (uint32_t)AUDIO_SAMPLE_RATE_EXACT) {
    if (configured_) {
        // Publish a fully initialized object to any already-installed callback.
        registered_ = true;
        AudioInputUSB *expected = nullptr;
        if (instance_.compare_exchange_strong(expected, this)) {
            usbAudio_.onData(&AudioInputUSB::onUsbData);
        } else {
            registered_ = false;
        }
    }
}

AudioInputUSB::~AudioInputUSB() {
    if (!registered_) {
        return;
    }

    AudioInputUSB *expected = this;
    if (instance_.compare_exchange_strong(expected, nullptr)) {
        // Keep the static callback installed: it becomes a no-op after the
        // instance pointer is cleared, avoiding a callback-pointer race.
        while (callbackCount_.load() != 0) {
            vTaskDelay(1);
        }
    }
}

void AudioInputUSB::setStreaming(bool enabled) {
    resetPending_.store(true, std::memory_order_release);
    discardPending_.store(true, std::memory_order_release);
    streaming_.store(enabled && isConfigured(), std::memory_order_release);
}

void AudioInputUSB::onUsbData(void *data, uint16_t len) {
    callbackCount_.fetch_add(1);
    AudioInputUSB *input = instance_.load();
    if (input != nullptr) {
        input->receiveData(data, len);
    }
    callbackCount_.fetch_sub(1);
}

void AudioInputUSB::receiveData(void *data, uint16_t len) {
    if (resetPending_.exchange(false, std::memory_order_acq_rel)) {
        fillFrames_ = 0;
    }

    constexpr uint32_t bytesPerFrame = 2U * sizeof(int16_t);
    if (!isConfigured() || !streaming_.load(std::memory_order_acquire) ||
        data == nullptr || len == 0 || (len % bytesPerFrame) != 0) {
        fillFrames_ = 0;
        return;
    }

    // Apply the host's speaker volume/mute controls before adding the samples.
    usbAudio_.applyVolume(data, len);
    const int16_t *source = (const int16_t *)data;
    const uint32_t frameCount = len / bytesPerFrame;
    uint32_t write = writeBlockIndex_.load(std::memory_order_relaxed);

    uint32_t framesRemaining = frameCount;

    while (framesRemaining != 0) {
        // Check capacity only when starting a new block.
        if (fillFrames_ == 0 &&
            write - readBlockIndex_.load(std::memory_order_acquire) >= RX_RING_BLOCKS) {
            return;
        }

        uint32_t framesToCopy = AUDIO_BLOCK_SAMPLES - fillFrames_;
        if (framesToCopy > framesRemaining) {
            framesToCopy = framesRemaining;
        }

        int16_t *dst = rxSampleRing_[write & (RX_RING_BLOCKS - 1U)] + 2U * fillFrames_;
        const uint32_t valuesToCopy = framesToCopy * 2U;
        memcpy(dst, source, valuesToCopy * sizeof(int16_t));

        source += valuesToCopy;
        fillFrames_ += framesToCopy;
        framesRemaining -= framesToCopy;

        if (fillFrames_ == AUDIO_BLOCK_SAMPLES) {
            fillFrames_ = 0;
            writeBlockIndex_.store(++write, std::memory_order_release);
        }
    }
}

OSPEED
void AudioInputUSB::update() {
    if (!isConfigured()) {
        return;
    }

    const uint32_t write = writeBlockIndex_.load(std::memory_order_acquire);

    // Discard queued samples when the host opens or closes the USB stream.
    if (discardPending_.exchange(false, std::memory_order_acq_rel)) {
        readBlockIndex_.store(writeBlockIndex_.load(std::memory_order_acquire), std::memory_order_release);
        return;
    }

    const uint32_t read = readBlockIndex_.load(std::memory_order_relaxed);
    if (!streaming_.load(std::memory_order_acquire)) {
        readBlockIndex_.store(write, std::memory_order_release);
        return;
    }
    if (read == write) {
        return;
    }

    audio_block_t *left = allocate();
    audio_block_t *right = left != nullptr ? allocate() : nullptr;
    if (left == nullptr || right == nullptr) {
        if (left != nullptr) {
            release(left);
        }
        if (right != nullptr) {
            release(right);
        }
        readBlockIndex_.store(read + 1U, std::memory_order_release);
        return;
    }

    const int16_t *source = rxSampleRing_[read & (RX_RING_BLOCKS - 1U)];
GCC_UNROLL
    for (uint32_t frame = 0; frame < AUDIO_BLOCK_SAMPLES; ++frame) {
        left->data[frame] = source[2U * frame];
        right->data[frame] = source[2U * frame + 1U];
    }

    readBlockIndex_.store(read + 1U, std::memory_order_release);

    // The host may have stopped the speaker interface while the block was copied.
    if (!streaming_.load(std::memory_order_acquire)) {
        release(left);
        release(right);
        return;
    }

    transmit(left, 0);
    transmit(right, 1);
    release(left);
    release(right);
}

#endif // SOC_USB_OTG_SUPPORTED && CONFIG_TINYUSB_AUDIO_ENABLED
