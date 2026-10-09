#include "sine_table.h"
#include "common.h"

extern "C" {
extern const int16_t sinTable_q15[513];
}

namespace AudioSineTable {

static inline int16_t sample(uint32_t phase, int32_t magnitude) {
    // 512 Intervalle pro Periode; Index liegt zwischen 0 und 511.
    const uint32_t index = phase >> 23;

    const int32_t val1 = sinTable_q15[index];
    const int32_t val2 = sinTable_q15[index + 1];

    // 16-Bit-Interpolationsfaktor aus den Phasenbits.
    const uint32_t scale = (phase >> 7) & 0xFFFFu;

    // Q16-Interpolation. Die zusätzliche Präzision bleibt bis zur
    // anschließenden Amplitudenskalierung erhalten.
    const int32_t interpolated =
        val1 * static_cast<int32_t>(0x10000u - scale) +
        val2 * static_cast<int32_t>(scale);

    // Entspricht der bisherigen multiply_32x32_rshift32()-Berechnung.
    const int64_t product =
        static_cast<int64_t>(interpolated) * magnitude;

    return static_cast<int16_t>(product >> 32);
}


OSPEED
void generate(int16_t* output,
              uint32_t& phase,
              uint32_t phaseIncrement,
              size_t count,
              int32_t magnitude) {
    for (size_t i = 0; i < count; ++i) {
        output[i] = sample(phase, magnitude);
        phase += phaseIncrement;
    }
}


OSPEED
void generatePhased(int16_t* output,
                    const uint32_t* phases,
                    size_t count,
                    int32_t magnitude) {
    for (size_t i = 0; i < count; ++i) {
        output[i] = sample(phases[i], magnitude);
    }
}

} // namespace AudioSineTable
