#include "AudioSampleBuffer.h"

#include <stdlib.h>

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
#endif

AudioSampleBuffer::~AudioSampleBuffer() {
    release();
}

void AudioSampleBuffer::release() {
    if (samples_ != nullptr) {
#if defined(ARDUINO_ARCH_ESP32)
        heap_caps_free(samples_);
#else
        free(samples_);
#endif
    }

    samples_ = nullptr;
    sample_count_ = 0;
    sample_rate_ = 0;
    is_psram_ = false;
}
