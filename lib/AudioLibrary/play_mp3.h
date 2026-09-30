#pragma once
#ifndef AUDIO_PLAY_MP3_H
#define AUDIO_PLAY_MP3_H

#include <Arduino.h>
#include <FS.h>

#include "softcodecs/AudioDecoderStream.h"
#include "softcodecs/AudioInputBuffer.h"
#include "softcodecs/AudioSource.h"
#include "softcodecs/AudioSourceFile.h"
#include "softcodecs/mp3/mp3dec.h"

class AudioPlayMp3 : public AudioDecoderStream
{
public:
    static constexpr int ERR_NONE = 0;
    static constexpr int ERR_FILE_NOT_FOUND = 1;
    static constexpr int ERR_OUT_OF_MEMORY = 2;
    static constexpr int ERR_FORMAT = 3;
    static constexpr int ERR_DECODER = 4;

    AudioPlayMp3();
    ~AudioPlayMp3() override;

    AudioPlayMp3(const AudioPlayMp3 &) = delete;
    AudioPlayMp3 &operator=(const AudioPlayMp3 &) = delete;

    bool play(const char *filename);

    bool play(
        fs::FS &fs,
        const char *filename
    );

    bool play(AudioSource &source);

    void stop();

    bool isPlaying() const;

    uint32_t positionMillis() const;
    uint32_t lengthMillis() const;

    int channels() const
    {
        return _channels;
    }

    int bitRate() const
    {
        return static_cast<int>(_bitrate);
    }

    int lastError() const
    {
        return _lastError;
    }

protected:
    DecodeResult decodePcmBuffer(
        int16_t *destination,
        size_t capacity,
        size_t &outSamples
    ) override;

private:
    bool startPlayback(
        AudioSource &source,
        bool takeOwnership
    );

    void closeSource();

    /*
     * Decoder-task-only input handling.
     */
    bool fillInput(size_t minimumBytes);
    bool appendInput(size_t requested);
    bool skipInput(size_t bytes);
    bool prepareMp3Input();

    bool validateFrameInfo(
        const MP3FrameInfo &info
    );

    static size_t id3TagSize(
        const uint8_t header[10]
    );

    AudioSource *_source = nullptr;
    bool _ownSource = false;

    AudioInputBuffer _inputBuffer;

    HMP3Decoder _decoder = nullptr;
    MP3FrameInfo _frameInfo = {};

    uint32_t _sampleRate = 0;
    uint16_t _channels = 0;
    uint32_t _bitrate = 0;

    uint64_t _dataSamplesPlayed = 0;

    bool _inputPrepared = false;
    volatile bool _paused = false;
    volatile int _lastError = ERR_NONE;
};

#endif
