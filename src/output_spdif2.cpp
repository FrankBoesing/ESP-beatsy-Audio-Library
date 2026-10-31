#include "output_spdif2.h"

AudioOutputSPDIF2::AudioOutputSPDIF2()
    : AudioOutputSPDIF(CONTROLLER, Pins{-1}) {}

AudioOutputSPDIF2::AudioOutputSPDIF2(const Pins &pins)
    : AudioOutputSPDIF(CONTROLLER, pins) {}

AudioOutputSPDIF2::AudioOutputSPDIF2(int8_t dout)
    : AudioOutputSPDIF(CONTROLLER, Pins{dout}) {}
