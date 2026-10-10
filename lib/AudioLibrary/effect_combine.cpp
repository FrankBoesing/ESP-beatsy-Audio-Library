#include "effect_combine.h"
#include "utility/dspinst.h"

// Audio blocks are 32-bit aligned. may_alias permits packed 32-bit access to
// the int16_t sample storage while keeping strict-aliasing rules satisfied.
typedef uint32_t audio_word_t __attribute__((__may_alias__));

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

    switch (mode_sel) {
        case ADD:
#pragma GCC unroll 4
            for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
                const int32_t a = blocka->data[i];
                const int32_t b = blockb->data[i];
                blocka->data[i] = saturate16(a + b);
            }
            break;
        case SUBTRACT:
#pragma GCC unroll 4
            for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
                const int32_t a = blocka->data[i];
                const int32_t b = blockb->data[i];
                blocka->data[i] = saturate16(a - b);
            }
            break;

        case OR: {
            audio_word_t *a = (audio_word_t *)blocka->data;
            const audio_word_t *b = (const audio_word_t *)blockb->data;
            constexpr size_t words = AUDIO_BLOCK_SAMPLES / 2;
#pragma GCC unroll 2
            for (size_t i = 0; i < words; ++i) {
                a[i] |= b[i];
            }
            break;
        }

        case XOR: {
            audio_word_t *a = (audio_word_t *)blocka->data;
            const audio_word_t *b = (const audio_word_t *)blockb->data;
            constexpr size_t words = AUDIO_BLOCK_SAMPLES / 2;
#pragma GCC unroll 2
            for (size_t i = 0; i < words; ++i) {
                a[i] ^= b[i];
            }
            break;
        }

        case AND: {
            audio_word_t *a = (audio_word_t *)blocka->data;
            const audio_word_t *b = (const audio_word_t *)blockb->data;
            constexpr size_t words = AUDIO_BLOCK_SAMPLES / 2;
#pragma GCC unroll 2
            for (size_t i = 0; i < words; ++i) {
                a[i] &= b[i];
            }
            break;
        }

        case MODULO: {
            audio_word_t *a = (audio_word_t *)blocka->data;
            const audio_word_t *b = (const audio_word_t *)blockb->data;
            constexpr size_t words = AUDIO_BLOCK_SAMPLES / 2;
#pragma GCC unroll 2
            for (size_t i = 0; i < words; ++i) {
                // Preserve Teensy's packed unsigned 32-bit modulo semantics.
                // A zero divisor yields zero instead of triggering division UB.
                a[i] = (b[i] == 0U) ? 0U : (a[i] % b[i]);
            }
            break;
        }
    }

    transmit(blocka);
    release(blocka);
    release(blockb);
}
