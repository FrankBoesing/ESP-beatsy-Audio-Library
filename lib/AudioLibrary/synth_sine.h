#ifndef synth_sine_h_
#define synth_sine_h_

#include "AudioStream.h"

class AudioSynthWaveformSine : public AudioStream {
public:
    AudioSynthWaveformSine()
        : AudioStream(0, nullptr),
          phase_accumulator(0),
          phase_increment(0),
          magnitude(16384) {}

    void frequency(float freq) {
        if (!(freq >= 0.0f)) {
            freq = 0.0f;
        } else if (freq > AUDIO_SAMPLE_RATE_EXACT / 2.0f) {
            freq = AUDIO_SAMPLE_RATE_EXACT / 2.0f;
        }

        phase_increment =
            static_cast<uint32_t>(
                freq * (4294967296.0f / AUDIO_SAMPLE_RATE_EXACT));
    }

    void phase(float angle) {
        if (!(angle >= 0.0f)) {
            angle = 0.0f;
        } else if (angle >= 360.0f) {
            angle -= 360.0f;

            if (angle >= 360.0f) {
                return;
            }
        }

        phase_accumulator =
            static_cast<uint32_t>(
                angle * (4294967296.0 / 360.0));
    }

    void amplitude(float n) {
        if (!(n >= 0.0f)) {
            n = 0.0f;
        } else if (n > 1.0f) {
            n = 1.0f;
        }

        magnitude = static_cast<int32_t>(n * 65536.0f);
    }

    void update() override;

private:
    uint32_t phase_accumulator;
    uint32_t phase_increment;

    // Q16 amplitude; default 16384 matches the Teensy implementation.
    int32_t magnitude;
};

#endif
