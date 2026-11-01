#ifndef AUDIO_BYTEORDER_H
#define AUDIO_BYTEORDER_H

#include <stdint.h>

// Make the selected ESP-IDF target visible even when this header is included
// before an Arduino/ESP-IDF header.
#if defined(__riscv) && defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#endif

// Reverse the order of the low 16 bits.
// Returns the byte-swapped 16-bit value in the low 16 bits of the result.
static inline uint32_t audio_rev16(uint32_t value) __attribute__((always_inline, unused));
static inline uint32_t audio_rev16(uint32_t value) {
#if defined(__riscv) && defined(__riscv_xlen) && (__riscv_xlen == 32) && \
    (defined(CONFIG_IDF_TARGET_ESP32S31) || \
     (defined(CONFIG_IDF_TARGET_ESP32P4) && defined(CONFIG_ESP32P4_REV_MIN_300)))
    uint32_t result = value;
    __asm__(
        ".option push\n"
        ".option arch, +zbb\n"
        "rev8 %0, %0\n"
        "srli %0, %0, 16\n"
        ".option pop\n"
        : "+r"(result)
    );
    return result;
#else
    return (uint32_t)__builtin_bswap16((uint16_t)value);
#endif
}

// Reverse the order of all 4 bytes.
static inline uint32_t audio_rev32(uint32_t value) __attribute__((always_inline, unused));
static inline uint32_t audio_rev32(uint32_t value) {
#if defined(__riscv) && defined(__riscv_xlen) && (__riscv_xlen == 32) && \
    (defined(CONFIG_IDF_TARGET_ESP32S31) || \
     (defined(CONFIG_IDF_TARGET_ESP32P4) && defined(CONFIG_ESP32P4_REV_MIN_300)))
    uint32_t result = value;
    __asm__(
        ".option push\n"
        ".option arch, +zbb\n"
        "rev8 %0, %0\n"
        ".option pop\n"
        : "+r"(result)
    );
    return result;
#else
    return __builtin_bswap32(value);
#endif
}

#endif // AUDIO_BYTEORDER_H
