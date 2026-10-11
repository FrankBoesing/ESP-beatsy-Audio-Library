#pragma once
#ifndef AUDIO_INPUT_USB_H
#define AUDIO_INPUT_USB_H

#include <Arduino.h>
#include <atomic>
#include <stdint.h>

#include "AudioStream.h"
#include "usb_audio_backend.h"

/**
 * USB audio source: receives playback audio sent by the USB host.
 *
 * It follows the AudioInputI2S model: output 0 is left and output 1 is right
 * in the default stereo configuration. Higher channel counts are reserved
 * for backends that support multichannel USB Audio descriptors.
 */
class AudioInputUSB : public AudioStream {
  public:
    explicit AudioInputUSB(uint8_t channels = 2);
    ~AudioInputUSB() override;

    bool begin();
    void end();

    bool isRunning() const;
    // True when host-to-device audio data has arrived recently.
    bool isStreaming() const;
    uint8_t channelCount() const { return channels_; }

    // Sets the graph-wide rate before USB audio is initialized.
    bool setSampleRate(float sampleRate);

  protected:
    void update() override;
    bool beginHardware() override;

  private:
    uint8_t channels_;
    bool registered_ = false;
    std::atomic<bool> enabled_{false};

    AudioInputUSB(const AudioInputUSB &) = delete;
    AudioInputUSB &operator=(const AudioInputUSB &) = delete;
};

#endif // AUDIO_INPUT_USB_H
