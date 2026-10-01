#pragma once
#ifndef SYNTH_WHITENOISE_H
#define SYNTH_WHITENOISE_H

#include "AudioStream.h"

class AudioSynthNoiseWhite : public AudioStream {
  public:
    AudioSynthNoiseWhite();

    void amplitude(float level);
    void update() override;

  private:
    int32_t _level;
    uint32_t _seed;

    static uint32_t _instanceCount;
};

#endif
