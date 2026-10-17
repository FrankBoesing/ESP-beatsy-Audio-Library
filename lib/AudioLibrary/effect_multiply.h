#ifndef effect_multiply_h_
#define effect_multiply_h_

#include "AudioStream.h"

#include <atomic>

class AudioEffectMultiply : public AudioStream {
  public:
    AudioEffectMultiply() : AudioStream(2, inputQueueArray) {}

    // Bypass forwards the signal unchanged. Changes take effect at the next audio block.
    void bypass(bool enable) { _bypassed.store(enable, std::memory_order_relaxed); }

    void update() override;

  private:
    std::atomic<bool> _bypassed{false};
    audio_block_t *inputQueueArray[2] = {};
};

#endif
