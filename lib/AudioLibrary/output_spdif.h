#pragma once
#ifndef AUDIO_OUTPUT_SPDIF_H
#define AUDIO_OUTPUT_SPDIF_H

#include <Arduino.h>
#include <atomic>
#include "i2s_port.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include "AudioStream.h"

/*
 * IEC 60958 / S/PDIF consumer PCM output, implemented by software BMC
 * encoding and the selected I2S standard-mode serializer. Public constructors
 * use I2S0; the protected controller-aware constructor enables future wrappers.
 *
 * One 16-bit stereo AudioStream block is encoded as 128 BMC half-bit states
 * per stereo sample frame. I2S therefore runs at 4x the AudioStream rate with
 * 16-bit stereo slots. Only the DATA output pin needs to be routed.
 *
 * This is a logic-level signal output. A coaxial S/PDIF connector requires
 * suitable line-driver / transformer circuitry; connect an optical transmitter
 * module only if its input levels and bandwidth are compatible.
 */
class AudioOutputSPDIF : public AudioStream {
  public:
    struct Pins {
        int8_t dout; // BMC-encoded S/PDIF signal output
    };

    AudioOutputSPDIF();
    explicit AudioOutputSPDIF(const Pins &pins);
    explicit AudioOutputSPDIF(int8_t dout);
    ~AudioOutputSPDIF() override;

    bool begin();
    bool begin(const Pins &pins);
    bool begin(int8_t dout);
    void end();

    bool isRunning() const;
    const Pins &pins() const { return spdifPins; }
    bool setSampleRate(float sampleRate);

  protected:
    // Shared controller-aware implementation point for future SPDIF2/3
    // wrappers. The public AudioOutputSPDIF constructors always select I2S0.
    AudioOutputSPDIF(uint8_t controller, const Pins &pins);

    void update() override;
    bool beginHardware() override;

  private:
    struct BlockPair {
        audio_block_t *left;
        audio_block_t *right;
        uint32_t rateGeneration = 0;
    };

    static constexpr size_t QUEUE_LENGTH = 4;

    Pins spdifPins{-1};
    uint8_t controller_ = 0;
    i2s_chan_handle_t txHandle = nullptr;
    QueueHandle_t txQueue = nullptr;
    // Prevent the TX task from writing during shared I2S clock reconfiguration.
    std::atomic<TaskHandle_t> txTask{nullptr};

    std::atomic<uint32_t> running{0};
    std::atomic<uint8_t> sampleRateCode{0};
    std::atomic<uint32_t> sampleRateCodeGeneration{0};

    // Reused exclusively by the TX task: four 16-bit I2S frames per PCM
    // sample per channel pair, i.e. eight halfwords per stereo PCM frame.
    alignas(4) uint16_t encodedBuffer[AUDIO_BLOCK_SAMPLES * 8U] = {};
    uint16_t frameInBlock = 0;

    static bool AUDIO_I2S_ISR_ATTR onI2STransmit(i2s_chan_handle_t channel, i2s_event_data_t *event,
                                                 void *userContext);
    static void txTaskEntry(void *arg);

    void txTaskLoop();

    bool beginInternal();
    void releasePair(BlockPair &pair);
    void setRunning(bool value);
    // This runs once for each PCM sample frame (128 times per block), so
    // keep it inline and avoid a function call in the real-time encoder loop.
    inline uint8_t channelStatusBit(uint16_t frame) const {
        if (frame >= 24U && frame < 28U) {
            const uint8_t rateCode = sampleRateCode.load(std::memory_order_relaxed);
            return static_cast<uint8_t>((rateCode >> (frame - 24U)) & 1U);
        }

        // The 16-bit word-length status code is 0x02, LSB-first: only frame
        // 33 carries a one. Avoid a variable shift for the other seven frames.
        return frame == 33U ? 1U : 0U;
    }
    void updateChannelStatus(float sampleRate);

    AudioOutputSPDIF(const AudioOutputSPDIF &) = delete;
    AudioOutputSPDIF &operator=(const AudioOutputSPDIF &) = delete;

    audio_block_t *inputQueueArray[2] = {};
};

#endif
