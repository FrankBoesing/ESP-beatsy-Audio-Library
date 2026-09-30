/*
 * Audio Library for ESP32
 * AudioMixer4 and AudioAmplifier
 */

#ifndef mixer_h_
#define mixer_h_

#include <Arduino.h>
#include "softcodecs/AudioStream.h"

class AudioMixer4 : public AudioStream
{
public:
    AudioMixer4(void) : AudioStream(4, inputQueueArray)
    {
        for (int i = 0; i < 4; i++) multiplier[i] = 65536;
    }

    virtual void update(void);

    void gain(unsigned int channel, float gain)
    {
        if (channel >= 4) return;
        if (gain > 32767.0f) gain = 32767.0f;
        else if (gain < -32767.0f) gain = -32767.0f;
        multiplier[channel] = (int32_t)(gain * 65536.0f);
    }

private:
    int32_t multiplier[4];
    audio_block_t *inputQueueArray[4];
};


class AudioAmplifier : public AudioStream
{
public:
    AudioAmplifier(void) : AudioStream(1, inputQueueArray), multiplier(65536)
    {
    }

    virtual void update(void);

    void gain(float n)
    {
        if (n > 32767.0f) n = 32767.0f;
        else if (n < -32767.0f) n = -32767.0f;
        multiplier = (int32_t)(n * 65536.0f);
    }

private:
    int32_t multiplier;
    audio_block_t *inputQueueArray[1];
};

#endif
