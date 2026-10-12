#pragma once
#ifndef AUDIO_SPDIF_BMC_ENCODER_H
#define AUDIO_SPDIF_BMC_ENCODER_H

#include <stdint.h>

namespace spdif_bmc {

// Encode one IEC 60958 stereo-channel subframe into four 16-bit I2S words.
// The destination words are ordered exactly as the MSB-first I2S serializer
// consumes them. This routine is shared by all potential SPDIF controller
// variants; it has no controller, queue, or AudioStream dependencies.
void encodeSubframe(int16_t sample, bool channelStatus, uint8_t preamble,
                    uint16_t *destination);

} // namespace spdif_bmc

#endif
