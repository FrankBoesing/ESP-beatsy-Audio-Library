#pragma once
#ifndef AUDIO_PLAY_AAC_H
#define AUDIO_PLAY_AAC_H

#include <Arduino.h>
#include <FS.h>

#include "Audio.h"
#include "softcodecs/AudioDecoderStream.h"
#include "softcodecs/AudioSource.h"
#include "softcodecs/AudioSourceFile.h"
#include "softcodecs/aac_decoder/aac_decoder.h"

class AudioPlayAac : public AudioDecoderStream {
  public:
    static constexpr int ERR_NONE = 0;
    static constexpr int ERR_FILE_NOT_FOUND = 1;
    static constexpr int ERR_OUT_OF_MEMORY = 2;
    static constexpr int ERR_FORMAT = 3;
    static constexpr int ERR_DECODER = 4;

    AudioPlayAac();
    ~AudioPlayAac() override;

    AudioPlayAac(const AudioPlayAac &) = delete;
    AudioPlayAac &operator=(const AudioPlayAac &) = delete;

    bool play(const char *filename);
    bool play(fs::FS &fs, const char *filename);
    bool play(AudioSource &source);

    void stop();
    bool isPlaying() const;

    uint32_t positionMillis() const;
    uint32_t lengthMillis() const;

#if SOFTCODEC_METRICS
    float decodeProcessorUsage() const;
    float decodeProcessorUsageMax() const;
    uint32_t decodeFrames() const;
    uint64_t decodeTimeUsTotal() const;
#endif

    int channels() const {
        return _channels;
    }

    int bitRate() const {
        return static_cast<int>(_bitrate);
    }

    int lastError() const {
        return _lastError;
    }

  protected:
    void onPlaybackFinished() override {
        _playing = false;
    }

    DecodeResult decodePcmBuffer(int16_t *destination, size_t capacity,
                                 size_t &outSamples) override;

  private:
    // Worst case: 2 channels x 1024 samples, doubled by SBR upsampling.
    static constexpr size_t AAC_PCM_BUFFER_SAMPLES = 4096U;
    static constexpr size_t AAC_INPUT_BUFFER_SIZE = 2048;
    static constexpr size_t AAC_MIN_FRAME_BYTES = 1536U + 8U;

    struct FrameInfo {
        int bitRate;
        int nChans;
        int sampRateOut;
        int bitsPerSample;
        int outputSamps;
    };

    bool startPlayback(AudioSource &source, bool takeOwnership);
    void closeSource();

    bool fillInput(size_t minimumBytes);
    bool skipInput(size_t bytes);
    bool prepareAacInput();
    bool validateFrameInfo(const FrameInfo &info);

    static size_t id3TagSize(const uint8_t header[10]);

    AudioSource *_source = nullptr;
    bool _ownSource = false;

    uint8_t _input[AAC_INPUT_BUFFER_SIZE] = {};
    size_t _inputPos = 0;
    size_t _inputLeft = 0;
    bool _inputEof = false;
    bool _inputPrepared = false;

    AACDecoder _decoder;

    uint32_t _sampleRate = 0;
    uint16_t _channels = 0;
    uint32_t _bitrate = 0;

#if SOFTCODEC_METRICS
    volatile uint32_t _decodeFrames = 0;
    volatile uint64_t _decodeTimeUsTotal = 0;
    volatile uint64_t _decodeAudioTimeUsTotal = 0;
    volatile uint32_t _decodeProcessorUsageMaxX100 = 0;
#endif

    volatile bool _paused = false;
    volatile bool _playing = false;
    volatile int _lastError = ERR_NONE;
};

#endif
