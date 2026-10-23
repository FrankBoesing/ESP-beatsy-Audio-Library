
#ifndef dspinst_h_
#define dspinst_h_

#include <stdint.h>
#include <math.h>

#define _INLINE_ __attribute__((always_inline, unused))

// Portable versions of the fixed-point DSP helpers used by the Audio Library.

static inline int CLZ(unsigned int x) _INLINE_;
static inline int CLZ(unsigned int x) { return x == 0 ? 32 : __builtin_clz(x); };

static inline unsigned int REV16(unsigned int value) _INLINE_;
static inline unsigned int REV16(unsigned int value) {
    return (unsigned int)__builtin_bswap16((unsigned short)value);
}; //FB
static inline unsigned int REV32(unsigned int value) _INLINE_;
static inline unsigned int REV32(unsigned int value) { return __builtin_bswap32(value); };
static inline int32_t FASTABS(int32_t x) _INLINE_;
static inline int32_t FASTABS(int32_t x) { return __builtin_abs(x); }
static inline uint64_t SAR64(uint64_t x, int32_t n) _INLINE_;
static inline uint64_t SAR64(uint64_t x, int32_t n) { return x >> n; }
static inline int32_t MULSHIFT32(int32_t x, int32_t y) _INLINE_;
static inline int32_t MULSHIFT32(int32_t x, int32_t y) {
    int32_t z;
    z = (uint64_t)x * (uint64_t)y >> 32;
    return z;
}
static inline uint64_t MADD64(uint64_t sum64, int32_t x, int32_t y) _INLINE_;
static inline uint64_t MADD64(uint64_t sum64, int32_t x, int32_t y) {
    sum64 += (uint64_t)x * (uint64_t)y; return sum64;
}

static inline int32_t signed_saturate_rshift(int32_t val, int bits, int rshift) _INLINE_;
static inline int32_t signed_saturate_rshift(int32_t val, int bits, int rshift) {
    int32_t out = val >> rshift;
    const int32_t max = (int32_t)1 << (bits - 1);
    if (out >= 0) {
        if (out > max - 1) out = max - 1;
    } else {
        if (out < -max) out = -max;
    }
    return out;
}

static inline int16_t saturate16(int32_t val) _INLINE_;
static inline int16_t saturate16(int32_t val) {
    val = (val > 32767) ? 32767 : val;
    val = (val < -32768) ? -32768 : val;
    return (int16_t)val;
}

static inline int32_t signed_multiply_32x16b(int32_t a, uint32_t b) _INLINE_;
static inline int32_t signed_multiply_32x16b(int32_t a, uint32_t b) {
    return (int32_t)(((int64_t)a * (int16_t)(b & 0xFFFF)) >> 16);
}

static inline int32_t signed_multiply_32x16t(int32_t a, uint32_t b) _INLINE_;
static inline int32_t signed_multiply_32x16t(int32_t a, uint32_t b) {
    return (int32_t)(((int64_t)a * (int16_t)(b >> 16)) >> 16);
}

static inline uint32_t pack_16b_16b(int32_t top, int32_t bottom) _INLINE_;
static inline uint32_t pack_16b_16b(int32_t top, int32_t bottom) {
    return ((uint32_t)(uint16_t)top << 16) | (uint16_t)bottom;
}

static inline int32_t multiply_32x32_rshift32(int32_t a, int32_t b) _INLINE_;
static inline int32_t multiply_32x32_rshift32(int32_t a, int32_t b) {
    return (int32_t)(((int64_t)a * (int64_t)b) >> 32);
}

static inline int32_t multiply_32x32_rshift32_rounded(int32_t a, int32_t b) _INLINE_;
static inline int32_t multiply_32x32_rshift32_rounded(int32_t a, int32_t b) {
    return (int32_t)((((int64_t)a * (int64_t)b) + INT64_C(0x80000000)) >> 32);

}

static inline int32_t multiply_accumulate_32x32_rshift32_rounded(int32_t sum, int32_t a, int32_t b) _INLINE_;
static inline int32_t multiply_accumulate_32x32_rshift32_rounded(int32_t sum, int32_t a, int32_t b) {
    return sum + (int32_t)((((int64_t)a * (int64_t)b) + INT64_C(0x80000000)) >> 32);
}

static inline int32_t multiply_subtract_32x32_rshift32_rounded(int32_t sum, int32_t a, int32_t b) _INLINE_;
static inline int32_t multiply_subtract_32x32_rshift32_rounded(int32_t sum, int32_t a, int32_t b) {
    return sum - (int32_t)((((int64_t)a * (int64_t)b) + INT64_C(0x80000000)) >> 32);
}

// Portable replacement for CMSIS arm_float_to_q31().
static inline int32_t float_to_q31(float value)  _INLINE_;
static inline int32_t float_to_q31(float value) {
    if (!isfinite(value)) {
        return 0;
    }
    if (value >= 1.0f) {
        return INT32_MAX;
    }
    if (value <= -1.0f) {
        return INT32_MIN;
    }

    return (int32_t)(value * 2147483648.0f);
}

static inline int32_t saturate_q31(int64_t value) _INLINE_;
static inline int32_t saturate_q31(int64_t value) {
    if (value > INT32_MAX) {
        return INT32_MAX;
    }
    if (value < INT32_MIN) {
        return INT32_MIN;
    }
    return (int32_t)value;
}

// Portable equivalent of CMSIS arm_add_q31() for one sample.
static inline int32_t add_q31_saturate(int32_t a, int32_t b) _INLINE_;
static inline int32_t add_q31_saturate(int32_t a, int32_t b) { return saturate_q31((int64_t)a + (int64_t)b); }

#ifdef __cplusplus
extern "C" {
#endif
extern const int16_t sinTable_q15[513];
#ifdef __cplusplus
}
#endif

constexpr size_t FAST_MATH_SIN_TABLE_SIZE = 512;

// Compatible with arm_sin_q15(): x is a fraction of a full turn in Q15 [0, 32768),
// the result is Q15. Uses a 512-step table with linear interpolation.
static inline int16_t sin_q15(int16_t x) _INLINE_;
static inline int16_t sin_q15(int16_t x) {
    const uint32_t phase = (uint32_t)x & 0x7FFF;
    const uint32_t index = phase >> 6;
    const int32_t s0 = sinTable_q15[index];
    const int32_t s1 = sinTable_q15[index + 1];
    return (int16_t)(s0 + (((s1 - s0) * (int32_t)(phase & 0x3F)) >> 6));
}

// Compatible with arm_cos_q15(): x is a fraction of a full turn in Q15 [0, 32768),
// the result is Q15. Uses the same 512-step table with linear interpolation.
static inline int16_t cos_q15(int16_t x) _INLINE_;
static inline int16_t cos_q15(int16_t x) {
    const uint32_t phase = ((uint32_t)x + 8192U) & 0x7FFF;
    const uint32_t index = phase >> 6;
    const int32_t s0 = sinTable_q15[index];
    const int32_t s1 = sinTable_q15[index + 1];
    return (int16_t)(s0 + (((s1 - s0) * (int32_t)(phase & 0x3F)) >> 6));
}

// High-resolution phase lookup using the 512-entry Q15 sine table.
//
// Input:
//   phase: 32-bit phase accumulator; one complete turn = 2^32
//
// Output:
//   Interpolated Q15 table value with 16 fractional bits.
//   The result is in Q15.16 format.
//
// Keeping the interpolation fraction avoids quantizing the phase
// to 15 bits before generating the audio sample.
static inline int32_t sin_q15_phase_q16(uint32_t phase) _INLINE_;

static inline int32_t sin_q15_phase_q16(uint32_t phase) {
    const uint32_t index = phase >> 23;
    const int32_t scale = (phase >> 7) & 0xFFFF;

    const int32_t s0 = sinTable_q15[index];
    const int32_t s1 = sinTable_q15[index + 1];

    return s0 * (0x10000 - scale) + s1 * scale;
}

/*
Integer square root using Newton iterations. Zero must be handled before
*/

static constexpr uint16_t sqrt_integer_guess_table[33] = {
    55109, 38968, 27555, 19484, 13778, 9742, 6889, 4871, 3445, 2436, 1723, 1218, 862, 609, 431, 305, 216,
    153,   108,   77,    54,    39,    27,   20,   14,   10,   7,    5,    4,    3,   2,   1,   0};

/*
#! /usr/bin/perl
use POSIX;
print "const uint16_t sqrt_integer_guess_table[33] = {\n";
for ($i=0; $i <= 32; $i++) {
	printf "%5d", ceil(sqrt((0xFFFFFFFF >> $i) * sqrt(2)/2 ));
	print "," if $i < 32;
	print "\n";
}
print "};\n";
*/

static inline uint32_t sqrt_uint32(uint32_t in) _INLINE_;
static inline uint32_t sqrt_uint32(uint32_t in) {
    if (in == 0) {
        return 0;
    }

    uint32_t n = sqrt_integer_guess_table[CLZ(in)];
    n = ((in / n) + n) / 2;
    n = ((in / n) + n) / 2;
    n = ((in / n) + n) / 2;
    return n;
}

// Faster approximate integer square root. Also safe for zero input.
static inline uint32_t sqrt_uint32_approx(uint32_t in) _INLINE_;
static inline uint32_t sqrt_uint32_approx(uint32_t in) {
    if (in == 0) {
        return 0;
    }

    uint32_t n = sqrt_integer_guess_table[CLZ(in)];
    n = ((in / n) + n) / 2;
    n = ((in / n) + n) / 2;
    return n;
}

#include "../common.h"
#include <string.h>

static inline __attribute__((optimize("O2"))) void copyPlanarToInterleaved(
    uint32_t *__restrict__ buffer, const int16_t *__restrict__ left, const int16_t *__restrict__ right)
{
    if (left && right) {
#pragma GCC unroll 2
        for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            const uint32_t l = (uint16_t)left[i];
            const uint32_t r = (uint16_t)right[i];
            buffer[i] = l | (r << 16);
        }
        return;
    }

    if (!left && !right) {
        memset(buffer, 0, sizeof(int32_t) * AUDIO_BLOCK_SAMPLES);
        return;
    }

    if (left) {
#pragma GCC unroll 2
        for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            const uint32_t l = (uint16_t)left[i];
            buffer[i] = l ;
        }
        return;
    }

    /* if(right) */ {
#pragma GCC unroll 2
        for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            const uint32_t r = (uint16_t)right[i];
            buffer[i] = (r << 16);
        }
        return;
    }
}


static inline __attribute__((optimize("O2"))) void copyPlanarToInterleavedMono(
    uint32_t *__restrict__ buffer, const int16_t *__restrict__ left, const int16_t *__restrict__ right)
{
if (left && right) {
#pragma GCC unroll 2
    for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        int32_t mono = ((int32_t)left[i] + (int32_t)right[i]) / 2;
        buffer[i] = (uint16_t)((int16_t)mono);
    }
    return;
}

if (left) {
#pragma GCC unroll 2
    for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        buffer[i] = (uint16_t)((int16_t)left[i]);
    }
    return;
}

/*
if (right) {
#pragma GCC unroll 2
    for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        buffer[i] = (uint16_t)((int16_t)right[i]);
    }
    return;
}
*/

// Weder left noch right vorhanden -> Puffer mit Nullen füllen.
memset(buffer, 0, AUDIO_BLOCK_SAMPLES * sizeof(uint32_t));

}

static inline __attribute__((optimize("O2"))) void copyInterleavedToPlanar(
    const uint32_t *__restrict__ src,
    int16_t *__restrict__ left,
    int16_t *__restrict__ right)
{
    if (left && right) {
#pragma GCC unroll 2
        for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            const uint32_t sample = src[i];
            left[i]  = (int16_t)(uint16_t)sample;
            right[i] = (int16_t)(uint16_t)(sample >> 16);
        }
        return;
    }

    if (!left && !right) {
        return;
    }

    if (left) {
#pragma GCC unroll 2
        for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            left[i] = (int16_t)(uint16_t)src[i];
        }
        return;
    }

    /* if (right) */
#pragma GCC unroll 2
    for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        right[i] = (int16_t)(uint16_t)(src[i] >> 16);
    }
}
#endif
