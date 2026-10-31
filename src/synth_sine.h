#ifndef synth_sine_h_
#define synth_sine_h_

#include "AudioStream.h"

class AudioSynthWaveformSine : public AudioStream {
  public:
    AudioSynthWaveformSine() : AudioStream(0, nullptr), phase_accumulator(0), phase_increment(0), magnitude(16384) {}

    void frequency(float freq) {
        const float currentSampleRate = sampleRate();
        if (!(freq >= 0.0f)) {
            freq = 0.0f;
        } else if (freq > currentSampleRate / 2.0f) {
            freq = currentSampleRate / 2.0f;
        }

        phase_increment = (uint32_t)(freq * (4294967296.0f / currentSampleRate));
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

        phase_accumulator = (uint32_t)(angle * (4294967296.0 / 360.0));
    }

    void amplitude(float n) {
        if (!(n >= 0.0f)) {
            n = 0.0f;
        } else if (n > 1.0f) {
            n = 1.0f;
        }

        magnitude = (int32_t)(n * 65536.0f);
    }

    void update() override;

  private:
    uint32_t phase_accumulator;
    uint32_t phase_increment;

    // Q16 amplitude; default 16384 matches the Teensy implementation.
    int32_t magnitude;
};

class AudioSynthWaveformSineHires : public AudioStream {
  public:
    AudioSynthWaveformSineHires()
        : AudioStream(0, nullptr), phase_accumulator(0), phase_increment(0), magnitude(16384) {}

    void frequency(float freq) {
        const float currentSampleRate = sampleRate();
        if (!(freq >= 0.0f)) {
            freq = 0.0f;
        } else if (freq > currentSampleRate / 2.0f) {
            freq = currentSampleRate / 2.0f;
        }

        phase_increment = (uint32_t)(freq * (4294967296.0f / currentSampleRate));
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

        phase_accumulator = (uint32_t)(angle * (4294967296.0 / 360.0));
    }

    void amplitude(float n) {
        if (!(n >= 0.0f)) {
            n = 0.0f;
        } else if (n > 1.0f) {
            n = 1.0f;
        }

        magnitude = (int32_t)(n * 65536.0f);
    }

    void update() override;

  private:
    uint32_t phase_accumulator;
    uint32_t phase_increment;

    // Q16: 65536 = 100 % Amplitude.
    int32_t magnitude;
};


/*
 * Linear frequency-modulated sine oscillator.
 * Input -32768 stops the phase, input 0 keeps the set frequency,
 * and input +32767 approaches twice the set frequency.
 */
class AudioSynthWaveformSineModulated : public AudioStream {
  public:
    AudioSynthWaveformSineModulated()
        : AudioStream(1, inputQueueArray), phase_accumulator(0),
          phase_increment(0), magnitude(16384) {}

    // The input can double the frequency, so the carrier is limited to Fs/4.
    void frequency(float freq) {
        const float currentSampleRate = sampleRate();
        if (!(freq >= 0.0f)) {
            freq = 0.0f;
        } else if (freq > currentSampleRate / 4.0f) {
            freq = currentSampleRate / 4.0f;
        }

        phase_increment = (uint32_t)(freq * (4294967296.0f / currentSampleRate));
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

        phase_accumulator = (uint32_t)(angle * (4294967296.0 / 360.0));
    }

    void amplitude(float n) {
        if (!(n >= 0.0f)) {
            n = 0.0f;
        } else if (n > 1.0f) {
            n = 1.0f;
        }

        magnitude = (int32_t)(n * 65536.0f);
    }

    void update() override;

  private:
    audio_block_t *inputQueueArray[1];
    uint32_t phase_accumulator;
    uint32_t phase_increment;
    int32_t magnitude;
};

#endif
