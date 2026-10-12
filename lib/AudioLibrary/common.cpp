#include "common.h"

// Immutable all-zero PCM block shared by audio components.
// Namespace-scope const data is placed in read-only storage (.rodata) on ESP32.
const int16_t audio_zeroblock[AUDIO_BLOCK_SAMPLES] = {};
