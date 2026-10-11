#pragma once
#ifndef AUDIO_OUTPUT_PDM_H
#define AUDIO_OUTPUT_PDM_H

#include <Arduino.h>
#include <atomic>
#include "i2s_port.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
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
    uint32_t txCallbackCount() const;

    // Last PCM block submitted to the PDM driver, for diagnostics.
    uint32_t txBlockCount() const { return txBlockCountValue.load(std::memory_order_relaxed); }
    int32_t lastPcmMin() const { return lastPcmMinValue.load(std::memory_order_relaxed); }
    int32_t lastPcmMax() const { return lastPcmMaxValue.load(std::memory_order_relaxed); }
    uint32_t lastPcmMeanAbs() const { return lastPcmMeanAbsValue.load(std::memory_order_relaxed); }
    uint32_t txWriteErrorCount() const { return txWriteErrorCountValue.load(std::memory_order_relaxed); }
    uint32_t txQueueDropCount() const { return txQueueDropCountValue.load(std::memory_order_relaxed); }

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
    QueueHandle_t txQueue = nullptr;
    // Serializes PCM writes against live PDM clock reconfiguration.
    SemaphoreHandle_t txWriteMutex = nullptr;
    TaskHandle_t txTask = nullptr;

    std::atomic<uint32_t> running{0};
    std::atomic<uint32_t> txCallbackCountValue{0};
    std::atomic<uint32_t> txBlockCountValue{0};
    std::atomic<int32_t> lastPcmMinValue{0};
    std::atomic<int32_t> lastPcmMaxValue{0};
    std::atomic<uint32_t> lastPcmMeanAbsValue{0};
    std::atomic<uint32_t> txWriteErrorCountValue{0};
    std::atomic<uint32_t> txQueueDropCountValue{0};

    static bool AUDIO_I2S_ISR_ATTR onI2STransmit(i2s_chan_handle_t channel,
                                                 i2s_event_data_t *event,
                                                 void *userContext);
    static void txTaskEntry(void *arg);

    void txTaskLoop();
    bool beginInternal();
    void releasePair(BlockPair &pair);
    void setRunning(bool value);
    void resetTxCallbackCount();

    AudioOutputPDM(const AudioOutputPDM &) = delete;
    AudioOutputPDM &operator=(const AudioOutputPDM &) = delete;

    audio_block_t *inputQueueArray[2] = {};
};

#endif
