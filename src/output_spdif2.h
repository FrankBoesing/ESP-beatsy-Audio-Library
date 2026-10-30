#pragma once
#ifndef AUDIO_OUTPUT_SPDIF2_H
#define AUDIO_OUTPUT_SPDIF2_H

#include "output_spdif.h"

// Convenience wrapper for the second hardware I2S controller (I2S_NUM_1).
// Requires a target SoC and board pin mapping that support this controller.
// All encoding, diagnostics and control behavior are inherited from
// AudioOutputSPDIF.
class AudioOutputSPDIF2 : public AudioOutputSPDIF {
  public:
    using Pins = AudioOutputSPDIF::Pins;
    static constexpr uint8_t CONTROLLER = 1;

    AudioOutputSPDIF2();
    explicit AudioOutputSPDIF2(const Pins &pins);
    explicit AudioOutputSPDIF2(int8_t dout);
};

#endif
