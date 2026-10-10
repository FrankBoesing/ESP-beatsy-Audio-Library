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

//TODO: support http (not https) streams

class AudioPlayAac : public AudioDecoderStream {
  public:
    static constexpr const char *MIME_TYPES[] = {"audio/aac", "audio/aacp", "audio/x-aac" /*, "audio/mp4"*/};
    static constexpr AudioPlayerInfo INFO = {AudioCodec::AAC, "AAC", MIME_TYPES,
                                             sizeof(MIME_TYPES) / sizeof(MIME_TYPES[0])};
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
    bool play(AudioSource &source) override;

    void stop() override;
    bool isPlaying() const override;

    uint32_t positionMillis() const override;
    uint32_t lengthMillis() const override;
    const AudioPlayerInfo &info() const override { return INFO; }

#if SOFTCODEC_METRICS
    float decodeProcessorUsage() const;
    float decodeProcessorUsageMax() const;
    bool hasDecoderMetrics() const override { return true; }
    uint32_t decodeFrames() const;
    uint64_t decodeTimeUsTotal() const;
#endif

    int channels() const { return _channels; }
    int bitRate() const { return _bitrate; }
    bool hasLastError() const override { return true; }
    int lastError() const override { return _lastError; }

  protected:
    void onPlaybackFinished() override { _playing = false; }

    DecodeResult decodePcmBuffer(int16_t *destination, size_t capacity, size_t &outSamples) override;

  private:
    static constexpr size_t AAC_PCM_BUFFER_SAMPLES = AAC_MAX_NCHANS * AAC_MAX_NSAMPS * 2U;
    static constexpr size_t AAC_INPUT_BUFFER_SIZE = 3072;
    static constexpr size_t AAC_MIN_FRAME_BYTES = AAC_MAINBUF_SIZE + 8U;
    static_assert(AAC_INPUT_BUFFER_SIZE > AAC_MIN_FRAME_BYTES, "AAC input buffer too small for one frame");

    bool startPlayback(AudioSource &source, bool takeOwnership);
    void closeSource();

    bool fillInput(size_t minimumBytes);
    bool skipInput(size_t bytes);
    bool prepareAacInput();
    bool validateFrameInfo(const AACFrameInfo &info);

    static size_t id3TagSize(const uint8_t header[10]);

    AudioSource *_source = nullptr;
    bool _ownSource = false;

    uint8_t _input[AAC_INPUT_BUFFER_SIZE] = {};
    size_t _inputPos = 0;
    size_t _inputLeft = 0;
    bool _inputEof = false;
    bool _inputPrepared = false;

    AACDecoder _decoder;
    AACFrameInfo _frameInfo = {};

    int32_t _sampleRate = 0;
    int32_t _channels = 0;
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
