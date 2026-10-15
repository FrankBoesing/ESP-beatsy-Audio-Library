#pragma once

#include <Arduino.h>

// Owns an interleaved-to-mono, signed 16-bit PCM sample buffer.
// AudioPlaySdWav allocates the samples in PSRAM; AudioPlayMemory reads them
// without copying. Keep this object alive for the entire playback.
class AudioSampleBuffer {
public:
    AudioSampleBuffer() = default;
    ~AudioSampleBuffer();

    AudioSampleBuffer(const AudioSampleBuffer &) = delete;
    AudioSampleBuffer &operator=(const AudioSampleBuffer &) = delete;

    void release();

    const int16_t *samples() const { return samples_; }
    uint32_t sampleCount() const { return sample_count_; }
    uint32_t sampleRate() const { return sample_rate_; }
    bool isValid() const { return samples_ != nullptr && sample_count_ != 0; }
    bool isInPsram() const { return is_psram_; }
    size_t sizeBytes() const { return (size_t)sample_count_ * sizeof(int16_t); }

private:
    friend class AudioPlaySdWav;

    int16_t *samples_ = nullptr;
    uint32_t sample_count_ = 0;
    uint32_t sample_rate_ = 0;
    bool is_psram_ = false;
};
