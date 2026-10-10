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
        // Do arithmetic per PCM sample, using 32 bits before saturating to int16.
        for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            const int32_t a = blocka->data[i];
            const int32_t b = blockb->data[i];
            const int32_t result = (mode == ADD) ? (a + b) : (a - b);
            blocka->data[i] = saturate16(result);
        }
    } else if (mode == MODULO) {
        // Keep the original Teensy semantics: MODULO operates on each packed
        // 32-bit pair of samples, not on individual int16_t samples.
        // memcpy copies one 32-bit word (two samples); it is only used here to
        // avoid violating strict-aliasing rules. Compilers normally optimize
        // these fixed-size copies to ordinary word loads/stores.
        for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; i += 2) {
            uint32_t a;
            uint32_t b;
            std::memcpy(&a, &blocka->data[i], sizeof(a));
            std::memcpy(&b, &blockb->data[i], sizeof(b));

            a = (b == 0U) ? 0U : (a % b);

            std::memcpy(&blocka->data[i], &a, sizeof(a));
        }
    } else {
        // Bitwise operations are independent for each sample, so there is no
        // need to pack pairs into uint32_t or use memcpy.
        for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            const int16_t a = blocka->data[i];
            const int16_t b = blockb->data[i];

            switch (mode) {
                case OR:
                    blocka->data[i] = static_cast<int16_t>(a | b);
                    break;
                case XOR:
                    blocka->data[i] = static_cast<int16_t>(a ^ b);
                    break;
                case AND:
                    blocka->data[i] = static_cast<int16_t>(a & b);
                    break;
                case MODULO:
                case ADD:
                case SUBTRACT:
                    // Handled by the dedicated paths above.
                    break;
            }
        }
    }

    transmit(blocka);
    release(blocka);
    release(blockb);
}
