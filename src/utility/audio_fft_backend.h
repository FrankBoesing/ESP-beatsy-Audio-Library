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

#ifndef AUDIO_FFT_BACKEND_H
#define AUDIO_FFT_BACKEND_H

#include <stdint.h>

namespace AudioFFT {

// Prepare shared twiddle tables and initialize ESP-DSP if it is available.
// Called by analyzer constructors so initialization does not occur in update().
void initialize();

// In-place forward complex FFT. data contains [Re0, Im0, Re1, Im1, ...].
// Supported lengths: 256 and 1024.
bool transform(float *data, uint16_t length);

// Return squared magnitude scaled to the legacy Teensy output convention:
// sqrt(power) ~= 16384 for a full-scale sine at its fundamental with no window.
uint32_t magnitudeSquaredScaled(const float *data, uint16_t bin,
                                float powerScale);

// sqrt helper with a zero guard. Returns a 16-bit magnitude value.
uint16_t magnitudeFromSquared(uint32_t power);

} // namespace AudioFFT

#endif // AUDIO_FFT_BACKEND_H
