/*
 * Copyright (c) 2016 Joao Rossi Filho
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

// Adapted from https://github.com/joaoRossiFilho/teensy_reverb

#include "effect_reverb.h"

#include <math.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <stdint.h>
#include <string.h>
#include "utility/dspinst.h"

static constexpr const char *TAG = "AudioEffectReverb";

AudioEffectReverb::~AudioEffectReverb() {
    free_lpf_buffers();
}

bool AudioEffectReverb::allocate_lpf_buffers(uint32_t capabilities) {
    lpf1_buf = (int32_t *)heap_caps_malloc((size_t)LPF1_BUF_LEN * sizeof(int32_t), capabilities);
    lpf2_buf = (int32_t *)heap_caps_malloc((size_t)LPF2_BUF_LEN * sizeof(int32_t), capabilities);
    lpf3_buf = (int32_t *)heap_caps_malloc((size_t)LPF3_BUF_LEN * sizeof(int32_t), capabilities);
    lpf4_buf = (int32_t *)heap_caps_malloc((size_t)LPF4_BUF_LEN * sizeof(int32_t), capabilities);

    if (lpf1_buf == nullptr || lpf2_buf == nullptr ||
        lpf3_buf == nullptr || lpf4_buf == nullptr) {
        free_lpf_buffers();
        return false;
    }

    return true;
}

void AudioEffectReverb::free_lpf_buffers() {
    if (lpf1_buf != nullptr) heap_caps_free(lpf1_buf);
    if (lpf2_buf != nullptr) heap_caps_free(lpf2_buf);
    if (lpf3_buf != nullptr) heap_caps_free(lpf3_buf);
    if (lpf4_buf != nullptr) heap_caps_free(lpf4_buf);

    lpf1_buf = nullptr;
    lpf2_buf = nullptr;
    lpf3_buf = nullptr;
    lpf4_buf = nullptr;
}

bool AudioEffectReverb::begin() {
    if (initialized) {
        return true;
    }

    // Prefer internal RAM for predictable access latency. We require all four
    // LPF buffers there; if any allocation fails, free the partial set and
    // retry the complete set in PSRAM.
    if (allocate_lpf_buffers(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) {
        using_psram = false;
    } else if (allocate_lpf_buffers(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) {
        using_psram = true;
        ESP_LOGD(TAG, "Internal RAM insufficient for reverb LPF buffers; using PSRAM");
    } else {
        using_psram = false;

        // update() may retry begin() on every audio cycle; report this only once
        // to avoid flooding the log while allocation remains unavailable.
        if (!allocation_error_logged) {
            ESP_LOGE(TAG, "Failed to allocate reverb LPF buffers in internal RAM and PSRAM");
            allocation_error_logged = true;
        }
        return false;
    }

    allocation_error_logged = false;
    init_comb_filters();
    clear_buffers();
    initialized = true;
    return true;
}

void AudioEffectReverb::_do_comb_apf(comb_apf *filter, int32_t *in_buf, int32_t *out_buf) {
    const int32_t g = filter->g;
    const uint32_t buf_len = filter->buf_len;
    uint32_t rd_idx = filter->rd_idx;
    uint32_t wr_idx = filter->wr_idx;

    for (uint32_t n = 0; n < AUDIO_BLOCK_SAMPLES; ++n) {
        int32_t acc_y = filter->buffer[rd_idx];
        int32_t acc_x = in_buf[n];

        const int32_t w = multiply_32x32_rshift32_rounded(g, acc_y);
        acc_x = saturate_q31((int64_t)acc_x + (int64_t)w * 2);

        const int32_t z = multiply_32x32_rshift32_rounded(g, acc_x);
        acc_y = saturate_q31((int64_t)acc_y - (int64_t)z * 2);

        filter->buffer[wr_idx] = acc_x;
        out_buf[n] = acc_y;

        if (++rd_idx >= buf_len) {
            rd_idx = 0;
        }
        if (++wr_idx >= buf_len) {
            wr_idx = 0;
        }
    }

    filter->rd_idx = rd_idx;
    filter->wr_idx = wr_idx;
}

void AudioEffectReverb::_do_comb_lpf(comb_lpf *filter, int32_t *in_buf, int32_t *out_buf) {
    const int32_t g1 = filter->g1;
    const int32_t g2 = filter->g2;
    int32_t z1 = filter->z1;
    const uint32_t buf_len = filter->buf_len;
    uint32_t rd_idx = filter->rd_idx;
    uint32_t wr_idx = filter->wr_idx;

    for (uint32_t n = 0; n < AUDIO_BLOCK_SAMPLES; ++n) {
        const int32_t y = filter->buffer[rd_idx];
        const int32_t x = in_buf[n];

        const int32_t w = multiply_accumulate_32x32_rshift32_rounded(y, g2, z1);
        const int32_t z = multiply_accumulate_32x32_rshift32_rounded(x, g1, w);

        z1 = w;
        filter->buffer[wr_idx] = z;
        out_buf[n] = y;

        if (++rd_idx >= buf_len) {
            rd_idx = 0;
        }
        if (++wr_idx >= buf_len) {
            wr_idx = 0;
        }
    }

    filter->z1 = z1;
    filter->rd_idx = rd_idx;
    filter->wr_idx = wr_idx;
}

void AudioEffectReverb::init_comb_filters() {
    g_flt_apf[0] = 0.7f;
    g_flt_apf[1] = -0.54f;
    g_flt_apf[2] = 0.6f;

    g2_flt_lpf = 0.985f;
    for (uint32_t i = 0; i < 3; ++i) {
        g_q31_apf[i] = float_to_q31(g_flt_apf[i]);
    }
    g2_q31_lpf = float_to_q31(g2_flt_lpf);

    apf[0].buffer = apf1_buf;
    apf[0].buf_len = APF1_BUF_LEN;
    apf[0].delay = APF1_DLY_LEN;

    apf[1].buffer = apf2_buf;
    apf[1].buf_len = APF2_BUF_LEN;
    apf[1].delay = APF2_DLY_LEN;

    apf[2].buffer = apf3_buf;
    apf[2].buf_len = APF3_BUF_LEN;
    apf[2].delay = APF3_DLY_LEN;

    for (uint32_t i = 0; i < 3; ++i) {
        apf[i].g = g_q31_apf[i];
        apf[i].wr_idx = 0;
        apf[i].rd_idx = apf[i].buf_len - apf[i].delay - 1U;
    }

    lpf[0].buffer = lpf1_buf;
    lpf[0].buf_len = LPF1_BUF_LEN;
    lpf[0].delay = LPF1_DLY_LEN;

    lpf[1].buffer = lpf2_buf;
    lpf[1].buf_len = LPF2_BUF_LEN;
    lpf[1].delay = LPF2_DLY_LEN;

    lpf[2].buffer = lpf3_buf;
    lpf[2].buf_len = LPF3_BUF_LEN;
    lpf[2].delay = LPF3_DLY_LEN;

    lpf[3].buffer = lpf4_buf;
    lpf[3].buf_len = LPF4_BUF_LEN;
    lpf[3].delay = LPF4_DLY_LEN;

    for (uint32_t i = 0; i < 4; ++i) {
        lpf[i].g1 = g1_q31_lpf[i];
        lpf[i].g2 = g2_q31_lpf;
        lpf[i].z1 = 0;
        lpf[i].wr_idx = 0;
        lpf[i].rd_idx = lpf[i].buf_len - lpf[i].delay - 1U;
    }
}

void AudioEffectReverb::clear_buffers() {
    memset(apf1_buf, 0, sizeof(apf1_buf));
    memset(apf2_buf, 0, sizeof(apf2_buf));
    memset(apf3_buf, 0, sizeof(apf3_buf));

    memset(lpf1_buf, 0, (size_t)LPF1_BUF_LEN * sizeof(int32_t));
    memset(lpf2_buf, 0, (size_t)LPF2_BUF_LEN * sizeof(int32_t));
    memset(lpf3_buf, 0, (size_t)LPF3_BUF_LEN * sizeof(int32_t));
    memset(lpf4_buf, 0, (size_t)LPF4_BUF_LEN * sizeof(int32_t));
}

void AudioEffectReverb::reverbTime(float seconds) {
    if (!isfinite(seconds) || seconds <= 0.0f) {
        return;
    }

    reverb_time_sec = seconds;

    g1_flt_lpf[0] = powf(10.0f, -(3.0f * LPF1_DLY_SEC) / reverb_time_sec);
    g1_flt_lpf[1] = powf(10.0f, -(3.0f * LPF2_DLY_SEC) / reverb_time_sec);
    g1_flt_lpf[2] = powf(10.0f, -(3.0f * LPF3_DLY_SEC) / reverb_time_sec);
    g1_flt_lpf[3] = powf(10.0f, -(3.0f * LPF4_DLY_SEC) / reverb_time_sec);

    for (uint32_t i = 0; i < 4; ++i) {
        g1_q31_lpf[i] = float_to_q31(g1_flt_lpf[i]);
        if (initialized) {
            lpf[i].g1 = g1_q31_lpf[i];
        }
    }
}

OSPEED
void AudioEffectReverb::update() {
    // The preferred path is to call begin() in setup(), before starting audio.
    // Keep existing sketches working by retrying initialization on first update.
    if (!initialized && !begin()) {
        // If both heaps are exhausted, don't interrupt the signal path.
        audio_block_t *input = receiveReadOnly();
        if (input != nullptr) {
            transmit(input);
            release(input);
        }
        return;
    }

    audio_block_t *block = receiveWritable();

    // Keep producing the reverb tail when no upstream block was available.
    if (block == nullptr) {
        block = allocate();
        if (block == nullptr) {
            return;
        }
        memset(block->data, 0, sizeof(block->data));
    }

    // Convert Q15 samples to Q31 with eight guard bits:
    // (sample * 2^16) >> 8 == sample * 256.
    for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        q31_buf[i] = (int32_t)block->data[i] * 256;
    }

    _do_comb_apf(&apf[0], q31_buf, q31_buf);
    _do_comb_apf(&apf[1], q31_buf, q31_buf);

    _do_comb_lpf(&lpf[0], q31_buf, sum_buf);
    for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        sum_buf[i] >>= 3;
    }

    _do_comb_lpf(&lpf[1], q31_buf, aux_buf);
    for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        aux_buf[i] >>= 3;
        sum_buf[i] = add_q31_saturate(sum_buf[i], aux_buf[i]);
    }

    _do_comb_lpf(&lpf[2], q31_buf, aux_buf);
    for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        aux_buf[i] >>= 3;
        sum_buf[i] = add_q31_saturate(sum_buf[i], aux_buf[i]);
    }

    _do_comb_lpf(&lpf[3], q31_buf, aux_buf);
    for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        aux_buf[i] >>= 3;
        sum_buf[i] = add_q31_saturate(sum_buf[i], aux_buf[i]);
    }

    _do_comb_apf(&apf[2], sum_buf, q31_buf);

    // Remove the eight guard bits with Q31 saturation, then convert to Q15.
    const int32_t sat_max = 0x007FFFFF;
    const int32_t sat_min = -0x00800000;
    for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        int32_t sample = q31_buf[i] >> 8;
        if (sample > sat_max) {
            sample = sat_max;
        } else if (sample < sat_min) {
            sample = sat_min;
        }

        block->data[i] = (int16_t)(sample >> 8);
    }

    transmit(block);
    release(block);
}
