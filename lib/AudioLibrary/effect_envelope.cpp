/* Audio Library for Teensy 3.X
 * Copyright (c) 2017, Paul Stoffregen, paul@pjrc.com
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

#include <Arduino.h>
#include "effect_envelope.h"

#define STATE_IDLE 0
#define STATE_DELAY 1
#define STATE_ATTACK 2
#define STATE_HOLD 3
#define STATE_DECAY 4
#define STATE_SUSTAIN 5
#define STATE_RELEASE 6
#define STATE_FORCED 7

static inline int32_t envelope_multiply_q16(int32_t gain, int16_t sample) { return (gain * (int32_t)sample) >> 16; }

void AudioEffectEnvelope::noteOn(void) {
    portENTER_CRITICAL(&_stateMux);
    if (state == STATE_IDLE || state == STATE_DELAY || release_forced_count == 0) {
        mult_hires = 0;
        count = delay_count;
        if (count > 0) {
            state = STATE_DELAY;
            inc_hires = 0;
        } else {
            state = STATE_ATTACK;
            count = attack_count;
            inc_hires = 0x40000000 / (int32_t)count;
        }
    } else if (state != STATE_FORCED) {
        state = STATE_FORCED;
        count = release_forced_count;
        inc_hires = (-mult_hires) / (int32_t)count;
    }
    ++_stateGeneration;
    portEXIT_CRITICAL(&_stateMux);
}

void AudioEffectEnvelope::noteOff(void) {
    portENTER_CRITICAL(&_stateMux);
    if (state != STATE_RELEASE && state != STATE_IDLE && state != STATE_FORCED) {
        state = STATE_RELEASE;
        count = release_count;
        inc_hires = (-mult_hires) / (int32_t)count;
    }
    ++_stateGeneration;
    portEXIT_CRITICAL(&_stateMux);
}

void AudioEffectEnvelope::update(void) {
    audio_block_t *block = receiveWritable();
    uint32_t *p = block ? (uint32_t *)(block->data) : NULL;
    uint32_t sample12, sample34, sample56, sample78;
    uint32_t tmp1, tmp2;
    uint8_t local_state;
    uint16_t local_count;
    int32_t local_mult_hires;
    int32_t local_inc_hires;
    uint16_t local_delay_count;
    uint16_t local_attack_count;
    uint16_t local_hold_count;
    uint16_t local_decay_count;
    int32_t local_sustain_mult;
    uint16_t local_release_count;
    uint32_t generation;

    portENTER_CRITICAL(&_stateMux);
    local_state = state;
    local_count = count;
    local_mult_hires = mult_hires;
    local_inc_hires = inc_hires;
    local_delay_count = delay_count;
    local_attack_count = attack_count;
    local_hold_count = hold_count;
    local_decay_count = decay_count;
    local_sustain_mult = sustain_mult;
    local_release_count = release_count;
    generation = _stateGeneration;
    portEXIT_CRITICAL(&_stateMux);

    if (local_state == STATE_IDLE) {
        if (block) {
            AudioStream::release(block);
        }
        return;
    }

    // Need to run the envelope process even with silent data, or
    // it gets stuck and never goes idle.
    for (unsigned int i = 0; i < AUDIO_BLOCK_SAMPLES / 8;) {
        // We only care about the state when completing a region.
        if (local_count == 0) {
            if (local_state == STATE_ATTACK) {
                local_count = local_hold_count;

                if (local_count > 0) {
                    local_state = STATE_HOLD;
                    local_mult_hires = 0x40000000;
                    local_inc_hires = 0;
                } else {
                    local_state = STATE_DECAY;
                    local_count = local_decay_count;
                    local_inc_hires = (local_sustain_mult - 0x40000000) / (int32_t)local_count;
                }
                continue;

            } else if (local_state == STATE_HOLD) {
                local_state = STATE_DECAY;
                local_count = local_decay_count;
                local_inc_hires = (local_sustain_mult - 0x40000000) / (int32_t)local_count;
                continue;

            } else if (local_state == STATE_DECAY) {
                local_state = STATE_SUSTAIN;
                local_count = 0xFFFF;
                local_mult_hires = local_sustain_mult;
                local_inc_hires = 0;

            } else if (local_state == STATE_SUSTAIN) {
                local_count = 0xFFFF;

            } else if (local_state == STATE_RELEASE) {
                local_state = STATE_IDLE;

                if (block) {
                    while (i < AUDIO_BLOCK_SAMPLES / 8) {
                        *p++ = 0;
                        *p++ = 0;
                        *p++ = 0;
                        *p++ = 0;
                        ++i;
                    }
                } else {
                    i = AUDIO_BLOCK_SAMPLES / 8;
                }
                break;

            } else if (local_state == STATE_FORCED) {
                local_mult_hires = 0;
                local_count = local_delay_count;

                if (local_count > 0) {
                    local_state = STATE_DELAY;
                    local_inc_hires = 0;
                } else {
                    local_state = STATE_ATTACK;
                    local_count = local_attack_count;
                    local_inc_hires = 0x40000000 / (int32_t)local_count;
                }

            } else if (local_state == STATE_DELAY) {
                local_state = STATE_ATTACK;
                local_count = local_attack_count;
                local_inc_hires = 0x40000000 / (int32_t)local_count;
                continue;
            }
        }

        if (block) {
            const int32_t gain = local_mult_hires >> 14;
            const int32_t inc = local_inc_hires >> 17;

            if (inc == 0) {
                // -----------------------------------------------------
                // Constant gain during this 8-sample region.
                // -----------------------------------------------------

                if (gain == 0) {
                    // Completely closed envelope.
                    *p++ = 0;
                    *p++ = 0;
                    *p++ = 0;
                    *p++ = 0;

                } else if (gain == 0x10000) {
                    // Unity gain: leave audio untouched.
                    p += 4;

                } else {
                    // Constant, non-unity gain.
                    sample12 = p[0];
                    sample34 = p[1];
                    sample56 = p[2];
                    sample78 = p[3];

                    tmp1 = envelope_multiply_q16(gain, (int16_t)(sample12 & 0xFFFFu));
                    tmp2 = envelope_multiply_q16(gain, (int16_t)(sample12 >> 16));
                    sample12 = pack_16b_16b(tmp2, tmp1);

                    tmp1 = envelope_multiply_q16(gain, (int16_t)(sample34 & 0xFFFFu));
                    tmp2 = envelope_multiply_q16(gain, (int16_t)(sample34 >> 16));
                    sample34 = pack_16b_16b(tmp2, tmp1);

                    tmp1 = envelope_multiply_q16(gain, (int16_t)(sample56 & 0xFFFFu));
                    tmp2 = envelope_multiply_q16(gain, (int16_t)(sample56 >> 16));
                    sample56 = pack_16b_16b(tmp2, tmp1);

                    tmp1 = envelope_multiply_q16(gain, (int16_t)(sample78 & 0xFFFFu));
                    tmp2 = envelope_multiply_q16(gain, (int16_t)(sample78 >> 16));
                    sample78 = pack_16b_16b(tmp2, tmp1);

                    p[0] = sample12;
                    p[1] = sample34;
                    p[2] = sample56;
                    p[3] = sample78;
                    p += 4;
                }

            } else {
                // -----------------------------------------------------
                // Ramp: gain changes for every sample.
                // -----------------------------------------------------
                int32_t mult = gain;

                sample12 = p[0];
                sample34 = p[1];
                sample56 = p[2];
                sample78 = p[3];

                mult += inc;
                tmp1 = envelope_multiply_q16(mult, (int16_t)(sample12 & 0xFFFFu));

                mult += inc;
                tmp2 = envelope_multiply_q16(mult, (int16_t)(sample12 >> 16));
                sample12 = pack_16b_16b(tmp2, tmp1);

                mult += inc;
                tmp1 = envelope_multiply_q16(mult, (int16_t)(sample34 & 0xFFFFu));

                mult += inc;
                tmp2 = envelope_multiply_q16(mult, (int16_t)(sample34 >> 16));
                sample34 = pack_16b_16b(tmp2, tmp1);

                mult += inc;
                tmp1 = envelope_multiply_q16(mult, (int16_t)(sample56 & 0xFFFFu));

                mult += inc;
                tmp2 = envelope_multiply_q16(mult, (int16_t)(sample56 >> 16));
                sample56 = pack_16b_16b(tmp2, tmp1);

                mult += inc;
                tmp1 = envelope_multiply_q16(mult, (int16_t)(sample78 & 0xFFFFu));

                mult += inc;
                tmp2 = envelope_multiply_q16(mult, (int16_t)(sample78 >> 16));
                sample78 = pack_16b_16b(tmp2, tmp1);

                p[0] = sample12;
                p[1] = sample34;
                p[2] = sample56;
                p[3] = sample78;
                p += 4;
            }
        } else {
            ++i;
        }

        // Keep the long-term gain at full 30-bit resolution.
        // This prevents cumulative rounding errors between blocks.
        local_mult_hires += local_inc_hires;
        --local_count;

        if (block) {
            ++i;
        }
    }

    portENTER_CRITICAL(&_stateMux);

    // A noteOn()/noteOff() happened while update() was running.
    // Do not overwrite the newer state.
    if (_stateGeneration != generation) {
        portEXIT_CRITICAL(&_stateMux);

        if (block) {
            AudioStream::release(block);
        }
        return;
    }

    state = local_state;
    count = local_count;
    mult_hires = local_mult_hires;
    inc_hires = local_inc_hires;

    ++_stateGeneration;
    portEXIT_CRITICAL(&_stateMux);

    if (block) {
        transmit(block);
        AudioStream::release(block);
    }
}

bool AudioEffectEnvelope::isActive() {
    portENTER_CRITICAL(&_stateMux);
    const bool active = state != STATE_IDLE;
    portEXIT_CRITICAL(&_stateMux);
    return active;
}

bool AudioEffectEnvelope::isSustain() {
    portENTER_CRITICAL(&_stateMux);
    const bool sustaining = state == STATE_SUSTAIN;
    portEXIT_CRITICAL(&_stateMux);
    return sustaining;
}
