/* Audio Library for Teensy 3.X
 * Copyright (c) 2014, Paul Stoffregen, paul@pjrc.com
 *
 * Development of this audio library was funded by PJRC.COM, LLC by sales of
 * Teensy and Audio Adaptor boards.  Please support PJRC's efforts to develop
 * open source software by purchasing Teensy or other PJRC products.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice, development funding notice, and this permission
 * notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef dspinst_h_
#define dspinst_h_

#include <stdint.h>

// Portable versions of the fixed-point DSP helpers used by the Audio Library.
// On Teensy/ARM these retain the original DSP instructions where available.

static inline int CLZ(unsigned int x) __attribute__((always_inline, unused));
static inline int CLZ(unsigned int x) { return x == 0 ? 32 : __builtin_clz(x); };

static inline unsigned int REV16(unsigned int value) __attribute__((always_inline, unused));
static inline unsigned int REV16(unsigned int value) {
    return (unsigned int)__builtin_bswap16((unsigned short)value);
}; //FB
static inline unsigned int REV32(unsigned int value) __attribute__((always_inline, unused));
static inline unsigned int REV32(unsigned int value) { return __builtin_bswap32(value); }; //FB
static inline int32_t FASTABS(int32_t x) __attribute__((always_inline, unused));
static inline int32_t FASTABS(int32_t x) { return __builtin_abs(x); } //xtensa has a fast abs instruction //fb
static inline uint64_t SAR64(uint64_t x, int32_t n) __attribute__((always_inline, unused));
static inline uint64_t SAR64(uint64_t x, int32_t n) { return x >> n; }
static inline int32_t MULSHIFT32(int32_t x, int32_t y) __attribute__((always_inline, unused));
static inline int32_t MULSHIFT32(int32_t x, int32_t y) {
    int32_t z;
    z = (uint64_t)x * (uint64_t)y >> 32;
    return z;
}
static inline uint64_t MADD64(uint64_t sum64, int32_t x, int32_t y) __attribute__((always_inline, unused));
static inline uint64_t MADD64(uint64_t sum64, int32_t x, int32_t y) {
    sum64 += (uint64_t)x * (uint64_t)y;
    return sum64;
}
static inline uint64_t xSAR64(uint64_t x, int32_t n) __attribute__((always_inline, unused));
static inline uint64_t xSAR64(uint64_t x, int32_t n) { return x >> n; }

static inline int32_t signed_saturate_rshift(int32_t val, int bits, int rshift) __attribute__((always_inline, unused));
static inline int32_t signed_saturate_rshift(int32_t val, int bits, int rshift) {
#if defined(__ARM_ARCH_7EM__)
    int32_t out;
    asm volatile("ssat %0, %1, %2, asr %3" : "=r"(out) : "I"(bits), "r"(val), "I"(rshift));
    return out;
#else
    int32_t out = val >> rshift;
    const int32_t max = (int32_t)1 << (bits - 1);
    if (out >= 0) {
        if (out > max - 1) out = max - 1;
    } else {
        if (out < -max) out = -max;
    }
    return out;
#endif
}

static inline int16_t saturate16(int32_t val) __attribute__((always_inline, unused));
static inline int16_t saturate16(int32_t val) {
#if defined(__ARM_ARCH_7EM__)
    int32_t tmp;
    asm volatile("ssat %0, %1, %2" : "=r"(tmp) : "I"(16), "r"(val));
    return (int16_t)tmp;
#else
    if (val > 32767)
        val = 32767;
    else if (val < -32768)
        val = -32768;
    return (int16_t)val;
#endif
}

static inline int32_t signed_multiply_32x16b(int32_t a, uint32_t b) __attribute__((always_inline, unused));
static inline int32_t signed_multiply_32x16b(int32_t a, uint32_t b) {
#if defined(__ARM_ARCH_7EM__)
    int32_t out;
    asm volatile("smulwb %0, %1, %2" : "=r"(out) : "r"(a), "r"(b));
    return out;
#else
    return (int32_t)(((int64_t)a * (int16_t)(b & 0xFFFF)) >> 16);
#endif
}

static inline int32_t signed_multiply_32x16t(int32_t a, uint32_t b) __attribute__((always_inline, unused));
static inline int32_t signed_multiply_32x16t(int32_t a, uint32_t b) {
#if defined(__ARM_ARCH_7EM__)
    int32_t out;
    asm volatile("smulwt %0, %1, %2" : "=r"(out) : "r"(a), "r"(b));
    return out;
#else
    return (int32_t)(((int64_t)a * (int16_t)(b >> 16)) >> 16);
#endif
}

static inline int32_t multiply_32x32_rshift32(int32_t a, int32_t b) __attribute__((always_inline, unused));
static inline int32_t multiply_32x32_rshift32(int32_t a, int32_t b) {
#if defined(__ARM_ARCH_7EM__)
    int32_t out;
    asm volatile("smmul %0, %1, %2" : "=r"(out) : "r"(a), "r"(b));
    return out;
#else
    return (int32_t)(((int64_t)a * (int64_t)b) >> 32);
#endif
}

static inline int32_t multiply_32x32_rshift32_rounded(int32_t a, int32_t b) __attribute__((always_inline, unused));
static inline int32_t multiply_32x32_rshift32_rounded(int32_t a, int32_t b) {
#if defined(__ARM_ARCH_7EM__)
    int32_t out;
    asm volatile("smmulr %0, %1, %2" : "=r"(out) : "r"(a), "r"(b));
    return out;
#else
    return (int32_t)((((int64_t)a * (int64_t)b) + INT64_C(0x80000000)) >> 32);
#endif
}

static inline int32_t multiply_accumulate_32x32_rshift32_rounded(int32_t sum, int32_t a, int32_t b)
    __attribute__((always_inline, unused));
static inline int32_t multiply_accumulate_32x32_rshift32_rounded(int32_t sum, int32_t a, int32_t b) {
#if defined(__ARM_ARCH_7EM__)
    int32_t out;
    asm volatile("smmlar %0, %2, %3, %1" : "=r"(out) : "r"(sum), "r"(a), "r"(b));
    return out;
#else
    return sum + (int32_t)((((int64_t)a * (int64_t)b) + INT64_C(0x80000000)) >> 32);
#endif
}

static inline int32_t multiply_subtract_32x32_rshift32_rounded(int32_t sum, int32_t a, int32_t b)
    __attribute__((always_inline, unused));
static inline int32_t multiply_subtract_32x32_rshift32_rounded(int32_t sum, int32_t a, int32_t b) {
#if defined(__ARM_ARCH_7EM__)
    int32_t out;
    asm volatile("smmlsr %0, %2, %3, %1" : "=r"(out) : "r"(sum), "r"(a), "r"(b));
    return out;
#else
    return sum - (int32_t)((((int64_t)a * (int64_t)b) + INT64_C(0x80000000)) >> 32);
#endif
}

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
static inline int16_t sin_q15(int16_t x) __attribute__((always_inline, unused));
static inline int16_t sin_q15(int16_t x) {
    const uint32_t phase = (uint32_t)x & 0x7FFF;
    const uint32_t index = phase >> 6;
    const int32_t s0 = sinTable_q15[index];
    const int32_t s1 = sinTable_q15[index + 1];
    return (int16_t)(s0 + (((s1 - s0) * (int32_t)(phase & 0x3F)) >> 6));
}

// Compatible with arm_cos_q15(): x is a fraction of a full turn in Q15 [0, 32768),
// the result is Q15. Uses the same 512-step table with linear interpolation.
static inline int16_t cos_q15(int16_t x) __attribute__((always_inline, unused));
static inline int16_t cos_q15(int16_t x) {
    const uint32_t phase = ((uint32_t)x + 8192U) & 0x7FFF;
    const uint32_t index = phase >> 6;
    const int32_t s0 = sinTable_q15[index];
    const int32_t s1 = sinTable_q15[index + 1];
    return (int16_t)(s0 + (((s1 - s0) * (int32_t)(phase & 0x3F)) >> 6));
}

#endif
