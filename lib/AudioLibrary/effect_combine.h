/*
 * AudioEffectDigitalCombine for ESP-beatsy Audio Library.
 *
 * Based on the Teensy Audio Library effect_combine implementation.
 * Original bitwise combine implementation by John-Michael Reed.
 *
 * The arithmetic modes ADD and SUBTRACT operate on signed 16-bit PCM
 * samples and saturate the result to the int16_t range.
 */

#ifndef effect_combine_h_
#define effect_combine_h_

#include "AudioStream.h"

#include <atomic>

class AudioEffectDigitalCombine : public AudioStream {
  public:
    enum combineMode {
        OR       = 0,
        XOR      = 1,
        AND      = 2,
        MODULO   = 3,
        ADD      = 4,
        SUBTRACT = 5, // Karaoke ;)
    };

    AudioEffectDigitalCombine() : AudioStream(2, inputQueueArray), mode_sel(OR) {}

    // Invalid values leave the current mode unchanged.
    void setCombineMode(int mode_in) {
        if (mode_in >= OR && mode_in <= SUBTRACT) {
            mode_sel = static_cast<combineMode>(mode_in);
        }
    }

    // Bypass forwards the signal unchanged. Changes take effect at the next audio block.
    void bypass(bool enable) { _bypassed.store(enable, std::memory_order_relaxed); }

    void update() override;

  private:
    std::atomic<bool> _bypassed{false};
    combineMode mode_sel;
    audio_block_t *inputQueueArray[2] = {};
};

#endif
