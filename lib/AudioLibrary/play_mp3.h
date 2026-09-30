#pragma once
#ifndef AUDIO_PLAY_MP3_H
#define AUDIO_PLAY_MP3_H

#include <Arduino.h>
#include <FS.h>

#include "softcodecs/AudioDecoderStream.h"
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
    static constexpr size_t MP3_PCM_BUFFER_SAMPLES =
        MAX_NCHAN * MAX_NGRAN * MAX_NSAMP;

    /*
     * Same compressed-input buffer size as the Teensy
     * AudioPlaySdMp3 implementation.
     *
     * The buffer is codec input, not a PCM buffer.
     */
    static constexpr size_t MP3_INPUT_BUFFER_SIZE = 2048;

    bool startPlayback(
        AudioSource &source,
        bool takeOwnership
    );

    void closeSource();

    /*
     * Decoder-task-only input handling.
     *
     * This deliberately follows the Teensy decoder's model:
     * one contiguous input buffer, compact remaining bytes, refill,
     * MP3FindSyncWord(), then MP3Decode(..., useSize=0).
     */
    bool fillInput(size_t minimumBytes);
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

    uint8_t _input[MP3_INPUT_BUFFER_SIZE] = {};
    size_t _inputPos = 0;
    size_t _inputLeft = 0;
    bool _inputEof = false;
    bool _inputPrepared = false;

    HMP3Decoder _decoder = nullptr;
    MP3FrameInfo _frameInfo = {};

    uint32_t _sampleRate = 0;
    uint16_t _channels = 0;
    uint32_t _bitrate = 0;

    volatile bool _paused = false;
    volatile int _lastError = ERR_NONE;
};

#endif
