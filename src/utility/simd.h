#ifndef AUDIO_SIMD_H
#define AUDIO_SIMD_H

#include <stdint.h>

// Make ESP-IDF target definitions available even when this header is included
// before an Arduino/ESP-IDF header.
#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#endif

// Shared 128-bit SIMD properties.
//
// ESP32-P4 uses Espressif's XespV extension on RISC-V.
// ESP32-S3 uses its Xtensa Processor Instruction Extensions (PIE).
// The instruction sets are different; only the data alignment and vector
// geometry are common.
#define AUDIO_SIMD_ALIGNMENT 16
#define AUDIO_SIMD_S16_LANES 8

#if defined(CONFIG_IDF_TARGET_ESP32P4) && defined(__riscv) && defined(__riscv_xlen) && (__riscv_xlen == 32)
#define AUDIO_SIMD_TARGET_P4 1
#else
#define AUDIO_SIMD_TARGET_P4 0
#endif

#if defined(CONFIG_IDF_TARGET_ESP32S3) && defined(__XTENSA__)
#define AUDIO_SIMD_TARGET_S3 1
#else
#define AUDIO_SIMD_TARGET_S3 0
#endif

#if AUDIO_SIMD_TARGET_P4 || AUDIO_SIMD_TARGET_S3
#define AUDIO_SIMD_ENABLED 1
#define AUDIO_SIMD_ALIGNAS alignas(AUDIO_SIMD_ALIGNMENT)
#else
#define AUDIO_SIMD_ENABLED 0
#define AUDIO_SIMD_ALIGNAS
#endif

#endif // AUDIO_SIMD_H
