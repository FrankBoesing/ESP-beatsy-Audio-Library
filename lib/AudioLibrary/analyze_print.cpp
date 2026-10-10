#include <Arduino.h>
#include <esp_log.h>

#include "analyze_print.h"

AudioAnalyzePrint::AudioAnalyzePrint(void)
    : AudioStream(1, inputQueueArray) {
}

void AudioAnalyzePrint::update(void) {
    audio_block_t *block = receiveReadOnly();

    if (block == nullptr) {
        return;
    }

    // Log only once globally, even if multiple instances exist.
    static bool reported = false;

    if (!reported) {
        reported = true;
        ESP_LOGE("AudioAnalyzePrint", "not implemented");
    }

    // This analyzer does not process or forward the audio.
    // Release the block so the audio memory pool remains available.
    release(block);
}

void AudioAnalyzePrint::trigger(void) {
    // Not implemented.
}

void AudioAnalyzePrint::trigger(float level, int edge) {
    (void)level;
    (void)edge;

    // Not implemented.
}
