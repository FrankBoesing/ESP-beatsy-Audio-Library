#include "effect_combine.h"

#include <cstring>

#include "utility/dspinst.h"

static_assert((AUDIO_BLOCK_SAMPLES % 2) == 0,
              "AudioEffectDigitalCombine requires an even number of samples per block");

OSPEED
void AudioEffectDigitalCombine::update() {
    audio_block_t *blocka = receiveWritable(0);
    audio_block_t *blockb = receiveReadOnly(1);

    if (blocka == nullptr || blockb == nullptr) {
        if (blocka != nullptr) {
            release(blocka);
        }
        if (blockb != nullptr) {
            release(blockb);
        }
        return;
    }

    const combineMode mode = mode_sel;

    if (mode == ADD || mode == SUBTRACT) {
        // PCM arithmetic: calculate in 32 bits, then saturate back to int16.
        for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            const int32_t a = blocka->data[i];
            const int32_t b = blockb->data[i];
            const int32_t result = (mode == ADD) ? (a + b) : (a - b);
            blocka->data[i] = saturate16(result);
        }
    } else {
        // The original Teensy implementation combines pairs of packed samples.
        // memcpy avoids strict-aliasing violations from casting int16_t* to uint32_t*.
        for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; i += 2) {
            uint32_t a;
            uint32_t b;
            std::memcpy(&a, &blocka->data[i], sizeof(a));
            std::memcpy(&b, &blockb->data[i], sizeof(b));

            switch (mode) {
                case OR:
                    a |= b;
                    break;
                case XOR:
                    a ^= b;
                    break;
                case AND:
                    a &= b;
                    break;
                case MODULO:
                    // MODULO retains the original unsigned 32-bit packed-sample
                    // semantics. Avoid division by zero for robustness.
                    a = (b == 0U) ? 0U : (a % b);
                    break;
                case ADD:
                case SUBTRACT:
                    // Handled by the sample-wise path above.
                    break;
            }

            std::memcpy(&blocka->data[i], &a, sizeof(a));
        }
    }

    transmit(blocka);
    release(blocka);
    release(blockb);
}
