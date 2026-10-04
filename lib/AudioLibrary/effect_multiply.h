#ifndef effect_multiply_h_
#define effect_multiply_h_

#include "AudioStream.h"

class AudioEffectMultiply : public AudioStream {
  public:
    AudioEffectMultiply() : AudioStream(2, inputQueueArray) {}

    void update() override;

  private:
    audio_block_t *inputQueueArray[2] = {};
};

#endif
