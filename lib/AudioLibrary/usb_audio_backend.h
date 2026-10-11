#pragma once
#ifndef AUDIO_USB_BACKEND_H
#define AUDIO_USB_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#ifndef AUDIO_USB_MAX_CHANNELS
#define AUDIO_USB_MAX_CHANNELS 8
#endif

#if AUDIO_USB_MAX_CHANNELS < 2
#error "AUDIO_USB_MAX_CHANNELS must be at least 2"
#endif

class AudioInputUSB;
class AudioOutputUSB;

namespace audio_usb_detail {
uint8_t normalizeChannelCount(uint8_t channels);
bool attachInput(AudioInputUSB *input, uint8_t channels);
void detachInput(AudioInputUSB *input);
void setInputEnabled(AudioInputUSB *input, bool enabled);
bool attachOutput(AudioOutputUSB *output, uint8_t channels);
void detachOutput(AudioOutputUSB *output);
void setOutputEnabled(AudioOutputUSB *output, bool enabled);
bool start(float sampleRate);
bool isStarted();
bool canChangeSampleRate();
bool inputStreaming();
bool outputStreaming();
size_t readInput(int16_t *interleaved, size_t frames);
void discardInput();
bool writeOutput(const int16_t *interleaved, size_t frames);
} // namespace audio_usb_detail

#endif // AUDIO_USB_BACKEND_H
