/* Audio Library for Teensy 3.X
 * Copyright (c) 2014, Paul Stoffregen, paul@pjrc.com
 *
 * Development of this audio library was funded by PJRC.COM, LLC by sales of
 * Teensy and Audio Adaptor boards. Please support PJRC's efforts to develop
 * open source software by purchasing other PJRC or Teensy products.
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

#include "audio_fft_backend.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "utility/dspinst.h"

// ESP-DSP is optional. If its headers and compiled component are present, use
// its radix-4 FFT (including the ESP32-S3 implementation when enabled by the
// component's build configuration). Otherwise use the portable fallback below.
#if defined(__has_include)
#  if __has_include("dsps_fft4r.h")
#    include "dsps_fft4r.h"
#    define AUDIO_FFT_HAVE_ESP_DSP 1
#  endif
#endif
#ifndef AUDIO_FFT_HAVE_ESP_DSP
#  define AUDIO_FFT_HAVE_ESP_DSP 0
#endif

namespace AudioFFT {
namespace {

static constexpr uint16_t kMaxFFTSize = 1024;
static constexpr uint16_t kMaxTwiddles = kMaxFFTSize / 2;
static constexpr float kPi = 3.14159265358979323846f;

struct TwiddleTable {
    float re[kMaxTwiddles];
    float im[kMaxTwiddles];

    TwiddleTable() {
        // Forward-FFT twiddles: exp(-j * 2*pi*k/N).
        for (uint16_t k = 0; k < kMaxTwiddles; ++k) {
            const float angle = -2.0f * kPi * static_cast<float>(k) /
                                static_cast<float>(kMaxFFTSize);
            re[k] = cosf(angle);
            im[k] = sinf(angle);
        }
    }
};

static const TwiddleTable &twiddles() {
    // C++11 local-static initialization is thread-safe. Tables are prepared
    // in the analyzer constructor, not on the first real-time FFT update.
    static const TwiddleTable table;
    return table;
}

#if AUDIO_FFT_HAVE_ESP_DSP
static bool espDspInitAttempted = false;
static bool espDspReady = false;

static bool initializeEspDSP() {
    if (espDspInitAttempted) {
        return espDspReady;
    }
    espDspInitAttempted = true;

    // ESP-DSP's radix-4 table_size is expressed in float words and is 2*N
    // for a table initialized for an N-point FFT.
    if (dsps_fft4r_initialized != 0) {
        espDspReady = (dsps_fft4r_w_table_fc32 != nullptr) &&
                      (dsps_fft4r_w_table_size >= (2 * kMaxFFTSize));
        return espDspReady;
    }

    const esp_err_t err = dsps_fft4r_init_fc32(nullptr, kMaxFFTSize);
    espDspReady = (err == ESP_OK) && (dsps_fft4r_initialized != 0) &&
                  (dsps_fft4r_w_table_fc32 != nullptr) &&
                  (dsps_fft4r_w_table_size >= (2 * kMaxFFTSize));
    return espDspReady;
}
#endif

static bool transformPortable(float *data, uint16_t n) {
    if (data == nullptr || (n != 256 && n != 1024)) {
        return false;
    }

    const TwiddleTable &table = twiddles();

    // Bit-reversal permutation. The data array stores interleaved complex
    // numbers, so each swap moves both real and imaginary components.
    uint16_t j = 0;
    for (uint16_t i = 1; i < n; ++i) {
        uint16_t bit = static_cast<uint16_t>(n >> 1);
        while ((j & bit) != 0) {
            j = static_cast<uint16_t>(j ^ bit);
            bit = static_cast<uint16_t>(bit >> 1);
        }
        j = static_cast<uint16_t>(j ^ bit);

        if (i < j) {
            const uint16_t ii = static_cast<uint16_t>(2U * i);
            const uint16_t jj = static_cast<uint16_t>(2U * j);
            float tmp = data[ii];
            data[ii] = data[jj];
            data[jj] = tmp;
            tmp = data[ii + 1];
            data[ii + 1] = data[jj + 1];
            data[jj + 1] = tmp;
        }
    }

    // Iterative radix-2 decimation-in-time FFT. Twiddles are precomputed for
    // N=1024; smaller transforms stride through the same table.
    for (uint16_t len = 2; len <= n; len = static_cast<uint16_t>(len << 1)) {
        const uint16_t half = static_cast<uint16_t>(len >> 1);
        const uint16_t twiddleStep = static_cast<uint16_t>(kMaxFFTSize / len);

        for (uint16_t base = 0; base < n; base = static_cast<uint16_t>(base + len)) {
            for (uint16_t k = 0; k < half; ++k) {
                const uint16_t tw = static_cast<uint16_t>(k * twiddleStep);
                const float wr = table.re[tw];
                const float wi = table.im[tw];

                const uint16_t ia = static_cast<uint16_t>(2U * (base + k));
                const uint16_t ib = static_cast<uint16_t>(2U * (base + k + half));

                const float ar = data[ia];
                const float ai = data[ia + 1];
                const float br = data[ib];
                const float bi = data[ib + 1];

                const float tr = wr * br - wi * bi;
                const float ti = wr * bi + wi * br;

                data[ia] = ar + tr;
                data[ia + 1] = ai + ti;
                data[ib] = ar - tr;
                data[ib + 1] = ai - ti;
            }
        }
    }

    return true;
}

} // namespace

void initialize() {
#if AUDIO_FFT_HAVE_ESP_DSP
    if (initializeEspDSP()) {
        return;
    }
#endif
    (void)twiddles();
}

bool transform(float *data, uint16_t length) {
    if (data == nullptr || (length != 256 && length != 1024)) {
        return false;
    }

#if AUDIO_FFT_HAVE_ESP_DSP
    if (initializeEspDSP()) {
        esp_err_t result;
#if defined(CONFIG_IDF_TARGET_ESP32S3) && \
    defined(dsps_fft4r_fc32_aes3_enabled) && \
    (dsps_fft4r_fc32_aes3_enabled == 1)
        // Explicitly use the S3 kernel when ESP-DSP was built with optimized
        // kernels enabled. Its table_size argument is in float words.
        result = dsps_fft4r_fc32_aes3_(data, length,
                                       dsps_fft4r_w_table_fc32,
                                       dsps_fft4r_w_table_size);
#else
        // ESP-DSP uses a DIF-style radix-4 transform: perform its bit-reversal
        // permutation AFTER the FFT to return bins to natural order.
        result = dsps_fft4r_fc32(data, length);
#endif
        if (result != ESP_OK) {
            return false;
        }

        // Bit reversal is performed after the successful transform, matching
        // Espressif's own FFT examples. Do not run the portable fallback on a
        // buffer that has already been transformed.
        return dsps_bit_rev4r_fc32(data, length) == ESP_OK;
    }
#endif

    return transformPortable(data, length);
}

uint32_t magnitudeSquaredScaled(const float *data, uint16_t bin,
                                float powerScale) {
    if (data == nullptr) {
        return 0;
    }

    const uint16_t index = static_cast<uint16_t>(2U * bin);
    const float re = data[index];
    const float im = data[index + 1];
    const float power = (re * re + im * im) * powerScale;

    if (!(power > 0.0f)) {
        return 0;
    }

    // For normalized int16 input, power is <= 32768^2 for both supported
    // FFT sizes. Keep the conversion guarded for arbitrary/custom windows.
    if (power >= 4294967040.0f) {
        return UINT32_MAX;
    }

    return static_cast<uint32_t>(power + 0.5f);
}

uint16_t magnitudeFromSquared(uint32_t power) {
    // sqrt_uint32_approx() indexes its guess table with __builtin_clz(), so
    // zero must be handled here.
    if (power == 0) {
        return 0;
    }

    const uint32_t magnitude = sqrt_uint32_approx(power);
    return (magnitude > UINT16_MAX) ? UINT16_MAX :
           static_cast<uint16_t>(magnitude);
}

} // namespace AudioFFT
