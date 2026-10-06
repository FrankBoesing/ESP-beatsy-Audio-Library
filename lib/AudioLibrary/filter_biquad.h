#pragma once
#ifndef FILTER_BIQUAD_H
#define FILTER_BIQUAD_H

#include "AudioStream.h"

#include <stdint.h>

class AudioFilterBiquad : public AudioStream {
  public:
    AudioFilterBiquad();

    void setCoefficients(uint32_t stage, const int *coefficients);
    void setCoefficients(uint32_t stage, const double *coefficients);

    void setLowpass(uint32_t stage, float frequency, float q = 0.7071f);
    void setHighpass(uint32_t stage, float frequency, float q = 0.7071f);
    void setBandpass(uint32_t stage, float frequency, float q = 1.0f);
    void setNotch(uint32_t stage, float frequency, float q = 1.0f);
    void setLowShelf(uint32_t stage, float frequency, float gain,
                     float slope = 1.0f);
    void setHighShelf(uint32_t stage, float frequency, float gain,
                      float slope = 1.0f);

    void update() override;

  private:
    static constexpr uint32_t MAX_STAGES = 4;

    struct Coefficients {
        int32_t b0;
        int32_t b1;
        int32_t b2;
        int32_t a1;
        int32_t a2;
    };

    struct State {
        int64_t s1;
        int64_t s2;
    };

    void storeCoefficients(uint32_t stage, const double coefficients[5]);

    audio_block_t *inputQueueArray[1] = {};
    Coefficients _coefficients[MAX_STAGES] = {};
    State _state[MAX_STAGES] = {};
    uint8_t _stageCount = 0;
    portMUX_TYPE _configMux = portMUX_INITIALIZER_UNLOCKED;
};

#endif
