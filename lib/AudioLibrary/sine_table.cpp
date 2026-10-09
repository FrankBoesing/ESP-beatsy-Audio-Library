#include "sine_table.h"
#include "common.h"
#include "utility/dspinst.h"

extern "C" {
extern const int16_t sinTable_q15[513];
}

namespace AudioSineTable {

static inline int16_t sample(uint32_t phase, int32_t magnitude) {
    const int32_t interpolated = sin_q15_phase_q16(phase);

    const int64_t product = static_cast<int64_t>(interpolated) * magnitude;

    return static_cast<int16_t>(product >> 32);
}

OSPEED
void generate(int16_t *output, uint32_t &phase, uint32_t phaseIncrement, size_t count, int32_t magnitude) {
    for (size_t i = 0; i < count; ++i) {
        output[i] = sample(phase, magnitude);
        phase += phaseIncrement;
    }
}

OSPEED
void generatePhased(int16_t *output, const uint32_t *phases, size_t count, int32_t magnitude) {
    for (size_t i = 0; i < count; ++i) {
        output[i] = sample(phases[i], magnitude);
    }
}

} // namespace AudioSineTable
