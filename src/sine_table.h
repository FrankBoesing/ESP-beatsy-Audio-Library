#pragma once

#include <stddef.h>
#include <stdint.h>

namespace AudioSineTable {

// Generiert einen Sinusblock.
// phase wird nach den Samples auf die nächste Phase fortgeschrieben.
// magnitude: Q16-Amplitudenfaktor, 65536 entspricht 1.0.
void generate(int16_t* output,
              uint32_t& phase,
              uint32_t phaseIncrement,
              size_t count,
              int32_t magnitude);

// Generiert einen Sinusblock aus bereits berechneten Phasen.
// Wird von AudioSynthWaveformModulated verwendet.
void generatePhased(int16_t* output,
                    const uint32_t* phases,
                    size_t count,
                    int32_t magnitude);

} // namespace AudioSineTable
