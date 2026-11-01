#ifndef AUDIO_CLZ_H
#define AUDIO_CLZ_H

#include <stdint.h>

// Make the selected ESP-IDF target visible even when this helper is included
// before an Arduino/ESP-IDF header.
#if defined(__riscv) && defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#endif

// Count leading zero bits in a 32-bit value.
// ESP32-P4 rev >= 3.0 and ESP32-S31 support Zbb even if global -march omits it.
// The local assembler option permits CLZ without changing the object's
// global architecture attributes. Use it on ESP32-S31 and ESP32-P4 rev >= 3.0.
static inline int audio_clz32(uint32_t x) __attribute__((always_inline, unused));
static inline int audio_clz32(uint32_t x) {
#if defined(__riscv) && defined(__riscv_xlen) && (__riscv_xlen == 32) && \
    (defined(CONFIG_IDF_TARGET_ESP32S31) || \
     (defined(CONFIG_IDF_TARGET_ESP32P4) && defined(CONFIG_ESP32P4_REV_MIN_300)))
    uint32_t result;
    __asm__(
        ".option push\n"
        ".option arch, +zbb\n"
        "clz %0, %1\n"
        ".option pop\n"
        : "=r"(result)
        : "r"(x)
    );
    return (int)result;
#else
    // __builtin_clz(0) is undefined; preserve the library's 32-bit behavior.
    return x == 0 ? 32 : __builtin_clz(x);
#endif
}

#endif // AUDIO_CLZ_H
