#pragma once
#ifndef AUDIO_SPDIF_BMC_ENCODER_H
#define AUDIO_SPDIF_BMC_ENCODER_H

#include <stdint.h>

namespace spdif_bmc {

// Fixed lookup tables are defined once in spdif_bmc_encoder.cpp, in
// read-only .rodata (flash-mapped on ESP32). The byte table packs the BMC
// half-bit pattern in bits 0..15 and that byte's parity in bit 16.
extern const uint32_t BMC_BYTE_TABLE[256];
extern const uint8_t BMC_NIBBLE_TABLE[16];

// Encode one IEC 60958 subframe into four 16-bit I2S words in serializer order.
// Keeping this small helper in the header lets the compiler inline it in the
// block encoder. It is controller-independent and shared by future SPDIF2/3
// wrappers as well as AudioOutputSPDIF.
#if defined(__GNUC__)
#define SPDIF_BMC_ALWAYS_INLINE inline __attribute__((always_inline))
#else
#define SPDIF_BMC_ALWAYS_INLINE inline
#endif
SPDIF_BMC_ALWAYS_INLINE
void encodeSubframe(int16_t sample, bool channelStatus, uint8_t preamble, uint16_t *destination) {
    if (destination == nullptr) return;

    const uint16_t sampleBits = static_cast<uint16_t>(sample);
    const uint32_t lowEntry = BMC_BYTE_TABLE[sampleBits & 0xFFU];
    const uint32_t highEntry = BMC_BYTE_TABLE[sampleBits >> 8U];

    const bool lowParity = ((lowEntry >> 16U) & 1U) != 0;
    const bool highParity = ((highEntry >> 16U) & 1U) != 0;

    // IEC 60958 bits 4..31 require even parity. The zero auxiliary field adds
    // no parity, so P is the XOR of the two sample-byte parities and C.
    const bool parityBit = lowParity ^ highParity ^ channelStatus;
    const uint8_t statusNibble = static_cast<uint8_t>((channelStatus ? 0x04U : 0U) | (parityBit ? 0x08U : 0U));

    const uint16_t lowCode = static_cast<uint16_t>(lowEntry);
    uint16_t highCode = static_cast<uint16_t>(highEntry);

    // Table entries assume a HIGH phase at byte start. An odd-parity byte
    // flips the ending BMC phase; invert the next table word when required.
    bool phaseHigh = true; // the preceding eight auxiliary bits are zero
    phaseHigh ^= lowParity;
    if (!phaseHigh) highCode = static_cast<uint16_t>(~highCode);
    phaseHigh ^= highParity;

    uint8_t tail = BMC_NIBBLE_TABLE[statusNibble];
    if (!phaseHigh) tail = static_cast<uint8_t>(~tail);

    // Pack directly into the DMA buffer: no per-bit loop and no 64-bit
    // temporary followed by a separate split/store pass.
    constexpr uint16_t zeroAuxBmc = 0x3333;
    destination[0] = static_cast<uint16_t>((static_cast<uint16_t>(preamble) << 8U) | (zeroAuxBmc >> 8U));
    destination[1] = static_cast<uint16_t>(((zeroAuxBmc & 0xFFU) << 8U) | (lowCode >> 8U));
    destination[2] = static_cast<uint16_t>((lowCode << 8U) | (highCode >> 8U));
    destination[3] = static_cast<uint16_t>((highCode << 8U) | tail);
}
#undef SPDIF_BMC_ALWAYS_INLINE

} // namespace spdif_bmc

#endif
