#pragma once
#ifndef AUDIO_OUTPUT_PDM_H
#define AUDIO_OUTPUT_PDM_H

#include <Arduino.h>
#include <atomic>
#include "i2s_port.h"
#include "AudioStream.h"

/*
 * Direct PDM-DAC output through the hardware PCM-to-PDM converter.
 *
 * The ESP-IDF driver consumes 16-bit PCM blocks; I2S0 generates the PDM
 * bitstream in hardware. In mono mode one DATA pin is used. In stereo mode
 * the target must support the two-line PDM-DAC mode and both DATA pins must
 * be connected to separate low-pass filters / amplifiers.
 */
class AudioOutputPDM : public AudioStream {
  public:
    struct Pins {
        int8_t dataLeft;
        int8_t dataRight = -1;
    };

    static constexpr uint8_t CONTROLLER = 0;

    AudioOutputPDM();
    explicit AudioOutputPDM(const Pins &pins);
    explicit AudioOutputPDM(int8_t data);
    ~AudioOutputPDM() override;

    bool begin();
    bool begin(const Pins &pins);
    bool begin(int8_t data);
    // Two DATA pins select stereo automatically: left first, right second.
    bool begin(int8_t dataLeft, int8_t dataRight);
    void end();

    bool isRunning() const;
    const Pins &pins() const { return pdmPins; }

    // Mono averages AudioStream inputs 0/1 onto dataLeft. Stereo keeps the
    // channels separate and requires both dataLeft and dataRight pins.
    bool setChannelCount(uint channels);
    uint channels() const { return numChannels; }
    bool setSampleRate(float sampleRate);

  protected:
    void update() override;
    bool beginHardware() override;

  private:
    struct BlockPair {
        audio_block_t *left;
        audio_block_t *right;
    };

    static constexpr size_t QUEUE_LENGTH = 4;

    Pins pdmPins{-1, -1};
    uint numChannels = 1;

    i2s_chan_handle_t txHandle = nullptr;
    // Accessed only by AudioTask and lifecycle methods under GraphUpdateLock.
    BlockPair txQueue[QUEUE_LENGTH]{};
    uint8_t txQueueHead = 0;
    uint8_t txQueueTail = 0;
    uint8_t txQueueCount = 0;
    std::atomic<int16_t *> pendingDmaBuffer{nullptr};

    std::atomic<uint32_t> running{0};

    static bool AUDIO_I2S_ISR_ATTR onI2STransmit(i2s_chan_handle_t channel,
                                                 i2s_event_data_t *event,
                                                 void *userContext);

    bool beginInternal();
    void releasePair(BlockPair &pair);
    bool enqueuePair(const BlockPair &pair);
    bool dequeuePair(BlockPair &pair);
    void setRunning(bool value);

    AudioOutputPDM(const AudioOutputPDM &) = delete;
    AudioOutputPDM &operator=(const AudioOutputPDM &) = delete;

    audio_block_t *inputQueueArray[2] = {};
};

#endif
