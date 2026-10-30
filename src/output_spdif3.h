#pragma once
#ifndef AUDIO_OUTPUT_SPDIF3_H
#define AUDIO_OUTPUT_SPDIF3_H

#include "output_spdif.h"

// Convenience wrapper for the third hardware I2S controller (I2S_NUM_2).
// Available only on SoCs that implement this controller and expose a usable
// DATA output pin. All encoding and control behavior is inherited from
// AudioOutputSPDIF.
class AudioOutputSPDIF3 : public AudioOutputSPDIF {
  public:
    using Pins = AudioOutputSPDIF::Pins;
    static constexpr uint8_t CONTROLLER = 2;

    AudioOutputSPDIF3();
    explicit AudioOutputSPDIF3(const Pins &pins);
    explicit AudioOutputSPDIF3(int8_t dout);
};

#endif
