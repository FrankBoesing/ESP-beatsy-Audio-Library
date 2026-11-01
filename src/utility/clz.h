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
// ESP32-P4 supports Zbb even when the global -march does not enable it.
// The local assembler option permits CLZ without changing the object's
// global architecture attributes. Keep this path restricted to ESP32-P4.
static inline int audio_clz32(uint32_t x) __attribute__((always_inline, unused));
static inline int audio_clz32(uint32_t x) {
#if defined(__riscv) && defined(CONFIG_IDF_TARGET_ESP32P4) && defined(__riscv_xlen) && (__riscv_xlen == 32)
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
