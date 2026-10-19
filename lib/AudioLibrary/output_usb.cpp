#include "output_usb.h"

#if SOC_USB_OTG_SUPPORTED && CONFIG_TINYUSB_AUDIO_ENABLED

#include <USBAudioCard.h>
#include <algorithm>

AudioOutputUSB::AudioOutputUSB(USBAudioCard &usbAudio)
    : AudioStream(2, inputQueueArray_),
      usbAudio_(usbAudio),
      configured_(usbAudio.micChannels() == UAC_MIC_STEREO),
      ring_{},
      readIndex_(0),
      writeIndex_(0),
      droppedSamples_(0),
      shortWrites_(0),
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

uint32_t AudioOutputUSB::availableSamples() const {
    const uint32_t read = readIndex_.load(std::memory_order_acquire);
    const uint32_t write = writeIndex_.load(std::memory_order_acquire);
    return write - read;
}

OSPEED
void AudioOutputUSB::update() {
    audio_block_t *blocks[2] = {receiveReadOnly(0), receiveReadOnly(1)};

    if (!configured_ || !streaming_.load(std::memory_order_acquire)) {
        // Discard queued data while capture is stopped, so restart never sends stale audio.
        readIndex_.store(writeIndex_.load(std::memory_order_acquire),
                         std::memory_order_release);
        if (blocks[0] != nullptr) {
            release(blocks[0]);
        }
        if (blocks[1] != nullptr) {
            release(blocks[1]);
        }
        return;
    }

    const uint32_t write = writeIndex_.load(std::memory_order_relaxed);
    const uint32_t read = readIndex_.load(std::memory_order_acquire);
    const uint32_t usedSamples = write - read;
    const uint32_t freeSamples = usedSamples < kRingSamples ? kRingSamples - usedSamples : 0;
    const uint32_t freeFrames = freeSamples / 2;
    const uint32_t copyFrames = std::min((uint32_t)AUDIO_BLOCK_SAMPLES, freeFrames);

    // Use the shared flash-resident zero block when either input is unconnected.
    const int16_t *left = blocks[0] != nullptr ? blocks[0]->data : audio_zeroblock;
    const int16_t *right = blocks[1] != nullptr ? blocks[1]->data : audio_zeroblock;

    // Stereo writes advance the producer index in pairs, so ringIndex is
    // always even and index can only be 0, 2, ..., 1022. The right sample
    // therefore remains inside the ring and needs no second mask operation.
    uint32_t ringIndex = write;
GCC_UNROLL
    for (uint32_t frame = 0; frame < copyFrames; ++frame, ringIndex += 2) {
        const uint32_t index = ringIndex & kRingMask;
        ring_[index] = left[frame];
        ring_[index + 1] = right[frame];
    }

    if (copyFrames != 0) {
        writeIndex_.store(write + copyFrames * 2, std::memory_order_release);
    }

    if (copyFrames < AUDIO_BLOCK_SAMPLES) {
        droppedSamples_.fetch_add(((uint32_t)AUDIO_BLOCK_SAMPLES - copyFrames) * 2,
                                  std::memory_order_relaxed);
    }

    if (blocks[0] != nullptr) {
        release(blocks[0]);
    }
    if (blocks[1] != nullptr) {
        release(blocks[1]);
    }

    // Drain at the AudioStream cadence instead of requiring loop() polling.
    // USBAudioCard::write() feeds TinyUSB's internal FIFO; USB packets transfer asynchronously.
    // Recheck streaming in case the host disabled capture during this update.
    if (!configured_ || !streaming_.load(std::memory_order_acquire)) {
        readIndex_.store(writeIndex_.load(std::memory_order_acquire),
                         std::memory_order_release);
        return;
    }

    const uint32_t writeAfterCopy = writeIndex_.load(std::memory_order_acquire);
    const uint32_t available = writeAfterCopy - read;
    if (available == 0) {
        return;
    }

    // Preserve channel-frame alignment through wraparound and partial writes.
    const uint32_t contiguous = kRingSamples - (read & kRingMask);
    uint32_t sampleCount = std::min(available,
        std::min((uint32_t)AUDIO_BLOCK_SAMPLES * 2, contiguous));
    sampleCount -= sampleCount % 2;
    if (sampleCount == 0) {
        return;
    }

    const uint16_t bytes = (uint16_t)(sampleCount * sizeof(int16_t));
    uint16_t written = usbAudio_.write(&ring_[read & kRingMask], bytes);

    const uint32_t samplesWritten = written / sizeof(int16_t);
    const uint16_t bytesPerFrame = 2 * sizeof(int16_t);
    if ((written % bytesPerFrame) != 0) {
        // TinyUSB accepted a partial PCM frame. Drop the remainder of this
        // submission so subsequent writes start at a clean channel-frame boundary.
        droppedSamples_.fetch_add(sampleCount - samplesWritten, std::memory_order_relaxed);
        readIndex_.store(read + sampleCount, std::memory_order_release);
        shortWrites_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (samplesWritten != 0) {
        readIndex_.store(read + samplesWritten, std::memory_order_release);
    }

    if (written != bytes) {
        shortWrites_.fetch_add(1, std::memory_order_relaxed);
    }
}

#endif // SOC_USB_OTG_SUPPORTED && CONFIG_TINYUSB_AUDIO_ENABLED
