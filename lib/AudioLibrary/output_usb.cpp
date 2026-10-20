#include "output_usb.h"

#if SOC_USB_OTG_SUPPORTED && CONFIG_TINYUSB_AUDIO_ENABLED

#include <USBAudioCard.h>
#include "esp32-hal-tinyusb.h"

AudioOutputUSB::AudioOutputUSB(USBAudioCard &usbAudio)
    : AudioStream(2, inputQueueArray_),
      usbAudio_(usbAudio),
      configured_(usbAudio.micChannels() == UAC_MIC_STEREO),
      interleaved_{},
      streaming_(false) {}

AudioOutputUSB::~AudioOutputUSB() = default;

void AudioOutputUSB::setStreaming(bool enabled) {
    streaming_.store(enabled && configured_, std::memory_order_release);
}

bool AudioOutputUSB::setGraphSampleRate(uint32_t sampleRateHz) {
    if (!configured_ || sampleRateHz == 0) {
        return false;
    }
    return AudioStream::setSampleRate((float)sampleRateHz);
}

OSPEED
void AudioOutputUSB::update() {
    audio_block_t *blocks[2] = {receiveReadOnly(0), receiveReadOnly(1)};

    if (!configured_ || !streaming_.load(std::memory_order_acquire)) {
        if (blocks[0] != nullptr) {
            release(blocks[0]);
        }
        if (blocks[1] != nullptr) {
            release(blocks[1]);
        }
        return;
    }

    // Use the shared flash-resident zero block when either input is unconnected.
    const int16_t *left = blocks[0] != nullptr ? blocks[0]->data : audio_zeroblock;
    const int16_t *right = blocks[1] != nullptr ? blocks[1]->data : audio_zeroblock;

    // Interleave one complete stereo block for TinyUSB's internal FIFO.
    uint32_t index = 0;
GCC_UNROLL
    for (uint32_t frame = 0; frame < AUDIO_BLOCK_SAMPLES; ++frame) {
        interleaved_[index++] = left[frame];
        interleaved_[index++] = right[frame];
    }

    if (blocks[0] != nullptr) {
        release(blocks[0]);
    }
    if (blocks[1] != nullptr) {
        release(blocks[1]);
    }

    // The host may have disabled capture while the block was being interleaved.
    if (!streaming_.load(std::memory_order_acquire)) {
        return;
    }

    const uint16_t bytes = (uint16_t)sizeof(interleaved_);
    const uint16_t written = usbAudio_.write(interleaved_, bytes);
    constexpr uint16_t bytesPerFrame = 2 * sizeof(int16_t);

    // A whole-frame short write drops the unaccepted tail of this block.
    // A partial stereo frame would desynchronize L/R, so clear the FIFO to
    // restore frame alignment; queued audio is sacrificed only on this error path.
    if ((written % bytesPerFrame) != 0) {
        (void)tud_audio_clear_ep_in_ff();
    }
}

#endif // SOC_USB_OTG_SUPPORTED && CONFIG_TINYUSB_AUDIO_ENABLED
