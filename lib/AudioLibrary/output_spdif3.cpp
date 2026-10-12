#include "output_spdif3.h"

AudioOutputSPDIF3::AudioOutputSPDIF3()
    : AudioOutputSPDIF(CONTROLLER, Pins{-1}) {}

AudioOutputSPDIF3::AudioOutputSPDIF3(const Pins &pins)
    : AudioOutputSPDIF(CONTROLLER, pins) {}

AudioOutputSPDIF3::AudioOutputSPDIF3(int8_t dout)
    : AudioOutputSPDIF(CONTROLLER, Pins{dout}) {}
