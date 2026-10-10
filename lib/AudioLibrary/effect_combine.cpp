#include "effect_combine.h"

#include "utility/dspinst.h"

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
        for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            const int32_t a = blocka->data[i];
            const int32_t b = blockb->data[i];
            const int32_t result = (mode == ADD) ? (a + b) : (a - b);
            blocka->data[i] = saturate16(result);
        }
    } else {
        // Preserve the original Teensy bitwise semantics by operating on
        // pairs of packed 16-bit samples as 32-bit words.
        uint32_t *pa = reinterpret_cast<uint32_t *>(blocka->data);
        const uint32_t *pb = reinterpret_cast<const uint32_t *>(blockb->data);
        const uint32_t *end = pa + AUDIO_BLOCK_SAMPLES / 2;

        switch (mode) {
            case OR:
                while (pa < end) {
                    *pa = *pa | *pb;
                    ++pa;
                    ++pb;
                }
                break;

            case XOR:
                while (pa < end) {
                    *pa = *pa ^ *pb;
                    ++pa;
                    ++pb;
                }
                break;

            case AND:
                while (pa < end) {
                    *pa = *pa & *pb;
                    ++pa;
                    ++pb;
                }
                break;

            case MODULO:
                // Preserve signed sample-wise modulo behavior while guarding
                // against a zero divisor. A zero divisor produces zero.
                for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
                    const int32_t a = blocka->data[i];
                    const int32_t b = blockb->data[i];
                    blocka->data[i] = (b == 0) ? 0 : static_cast<int16_t>(a % b);
                }
                break;

            case ADD:
            case SUBTRACT:
                // Handled above.
                break;
        }
    }

    transmit(blocka);
    release(blocka);
    release(blockb);
}
