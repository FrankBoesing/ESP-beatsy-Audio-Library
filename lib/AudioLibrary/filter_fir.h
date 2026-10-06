#pragma once
#ifndef FILTER_FIR_H
#define FILTER_FIR_H

#include "AudioStream.h"

#include <stddef.h>
#include <stdint.h>

#define FIR_PASSTHRU ((const short *)1)
#define FIR_MAX_COEFFS 200

class AudioFilterFIR : public AudioStream {
  public:
    AudioFilterFIR();

    void begin(const short *coefficients, int coefficientCount);
    void end();
    void update() override;

  private:
    enum class Mode : uint8_t { OFF, BYPASS, ACTIVE };

    static bool validCoefficientCount(int coefficientCount);

    audio_block_t *inputQueueArray[1] = {};

    short _coefficients[FIR_MAX_COEFFS] = {};
    int16_t _history[AUDIO_BLOCK_SAMPLES + FIR_MAX_COEFFS] = {};
    size_t _coefficientCount = 0;
    Mode _mode = Mode::OFF;
    bool _resetHistory = true;
    uint32_t _configurationGeneration = 0;
    portMUX_TYPE _configMux = portMUX_INITIALIZER_UNLOCKED;
};

#endif
