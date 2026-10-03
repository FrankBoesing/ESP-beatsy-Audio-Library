/*
 *  aac_decoder.h
 *  faad2 - ESP32 adaptation
 *  Created on: 12.09.2023
 *  Updated on: 13.08.2024
*/


#pragma once

#include <stdint.h>
#include <time.h>
#pragma GCC diagnostic warning "-Wunused-function"

struct AudioSpecificConfig {
    uint8_t audioObjectType;
    uint8_t samplingFrequencyIndex;
    uint8_t channelConfiguration;
};

// Each instance owns its complete decoder state; instances are independent.
class AACDecoder {
  public:
    AACDecoder() = default;
    ~AACDecoder() { FreeBuffers(); }

    AACDecoder(const AACDecoder &) = delete;
    AACDecoder &operator=(const AACDecoder &) = delete;

    bool        IsInit() const;
    bool        AllocateBuffers();
    void        FreeBuffers();
    uint8_t     AACGetFormat();
    uint8_t     AACGetParametricStereo();
    uint8_t     AACGetSBR();
    int         AACFindSyncWord(uint8_t *buf, int nBytes);
    int         AACSetRawBlockParams(int nChans, int sampRateCore, int profile);
    int16_t     AACGetOutputSamps();
    int         AACGetBitrate();
    int         AACGetChannels();
    int         AACGetSampRate();
    int         AACGetBitsPerSample();
    int         AACDecode(uint8_t *inbuf, int32_t *bytesLeft, short *outbuf);
    const char* AACGetErrorMessage(int8_t err);

  private:
    void *hAac = nullptr;  // NeAACDecHandle; libfaad's header leaks macros, so it stays out of here
    uint8_t frameHeaderType = 0;
    uint8_t frameSbr = 0;
    uint8_t framePS = 0;
    bool f_decoderIsInit = false;
    bool f_firstCall = false;
    bool f_setRaWBlockParams = false;
    uint32_t aacSamplerate = 0;
    uint8_t aacChannels = 0;
    uint8_t aacProfile = 0;
    uint16_t validSamples = 0;
    clock_t before = 0;
    float compressionRatio = 1;
};
