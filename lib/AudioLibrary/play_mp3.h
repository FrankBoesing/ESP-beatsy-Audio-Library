#pragma once
#ifndef AUDIO_PLAY_MP3_H
#define AUDIO_PLAY_MP3_H

#include <Arduino.h>
#include <FS.h>

#include "Audio.h"
#include "softcodecs/AudioDecoderStream.h"
#include "softcodecs/AudioSource.h"
#include "softcodecs/AudioSourceFile.h"
#include "softcodecs/mp3_decoder/mp3_decoder.h"

class AudioPlayMp3 : public AudioDecoderStream {
  public:
    static constexpr int ERR_NONE = 0;
    static constexpr int ERR_FILE_NOT_FOUND = 1;
    static constexpr int ERR_OUT_OF_MEMORY = 2;
    static constexpr int ERR_FORMAT = 3;
    static constexpr int ERR_DECODER = 4;
    static constexpr int ERR_SOURCE = 5;

    AudioPlayMp3();
    ~AudioPlayMp3() override;

    AudioPlayMp3(const AudioPlayMp3 &) = delete;
    AudioPlayMp3 &operator=(const AudioPlayMp3 &) = delete;

    bool play(const char *filename);
    bool play(fs::FS &fs, const char *filename);
    bool play(AudioSource &source);
    void stop();
    bool isPlaying() const;

    uint32_t positionMillis() const;
    uint32_t lengthMillis() const;

// Decoder diagnostics used by the example/test application.
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
        return (int)(_bitrate);
    }

    int lastError() const {
        return _lastError;
    }

  protected:
    void onPlaybackFinished() override {
        _playing = false;
    }

    DecodeResult decodePcmBuffer(int16_t *destination, size_t capacity, size_t &outSamples) override;

  private:
    static constexpr size_t MP3_PCM_BUFFER_SAMPLES = m_MAX_NCHAN * m_MAX_NGRAN * m_MAX_NSAMP;
    static constexpr size_t MP3_INPUT_BUFFER_SIZE = 2U * 1024U;
    static constexpr size_t MP3_STREAM_REFILL_TARGET = 1U * 1024U;

    bool startPlayback(AudioSource &source, bool takeOwnership);

    void closeSource();

    bool fillInput(size_t minimumBytes);
    bool skipInput(size_t bytes);
    bool prepareMp3Input();

    bool validateFrameInfo(const MP3FrameInfo &info);

    static size_t id3TagSize(const uint8_t header[10]);

    AudioSource *_source = nullptr;
    bool _ownSource = false;

    uint8_t _input[MP3_INPUT_BUFFER_SIZE] = {};
    size_t _inputPos = 0;
    size_t _inputLeft = 0;
    bool _inputEof = false;
    bool _inputPrepared = false;

    MP3Decoder _decoder;
    MP3FrameInfo _frameInfo = {};

    uint32_t _sampleRate = 0;
    uint16_t _channels = 0;
    uint32_t _bitrate = 0;

#if SOFTCODEC_METRICS
    // Decoder timing statistics. Updated only by the decoder task.
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
