#pragma once

#include <Arduino.h>
#include "AudioStream.h"
#include "AudioSource.h"
#include "AudioInputBuffer.h"

#include "mp3/mp3dec.h"


#ifndef AUDIO_MP3_INPUT_BUFFER_SIZE
#define AUDIO_MP3_INPUT_BUFFER_SIZE (32 * 1024)
#endif


class AudioPlayMp3 : public AudioStream
{
public:

    AudioPlayMp3();

    ~AudioPlayMp3() override;


    // ------------------------------------------------------------------------
    // Playback
    // ------------------------------------------------------------------------

    bool play(
        AudioSource &source
    );

    void stop();


    // ------------------------------------------------------------------------
    // Status
    // ------------------------------------------------------------------------

    bool isPlaying() const;

    bool isStopped() const;


    // ------------------------------------------------------------------------
    // AudioStream
    // ------------------------------------------------------------------------

    void update() override;


private:

    enum State : uint8_t {
        STOPPED,
        PLAYING
    };

    State state;


    // ------------------------------------------------------------------------
    // Source
    // ------------------------------------------------------------------------

    AudioSource *source;

    bool own_source;


    // ------------------------------------------------------------------------
    // Input buffer
    // ------------------------------------------------------------------------

    AudioInputBuffer inputBuffer;


    // ------------------------------------------------------------------------
    // MP3 decoder
    // ------------------------------------------------------------------------

    HMP3Decoder decoder;

    MP3FrameInfo frameInfo;


    // ------------------------------------------------------------------------
    // Decoder output
    // ------------------------------------------------------------------------

    /*
     * MP3Decode() liefert maximal:
     *
     * MPEG1:
     *   2 granules * 576 samples * 2 channels
     *
     * = 2304 int16_t
     */
    static constexpr size_t MAX_MP3_OUTPUT_SAMPLES =
        MAX_NGRAN *
        MAX_NSAMP *
        MAX_NCHAN;

    int16_t decodeBuffer[
        MAX_MP3_OUTPUT_SAMPLES
    ];


    size_t decodePosition;

    size_t decodeSamples;


    // ------------------------------------------------------------------------
    // Stream format
    // ------------------------------------------------------------------------

    uint32_t sample_rate;

    uint16_t channels;


    // ------------------------------------------------------------------------
    // Internal state
    // ------------------------------------------------------------------------

    bool source_eof;

    bool decoder_eof;


    // ------------------------------------------------------------------------
    // Playback
    // ------------------------------------------------------------------------

    bool startPlayback(
        AudioSource &source,
        bool takeOwnership
    );

    void closeSource();

    void resetDecoder();


    // ------------------------------------------------------------------------
    // Input
    // ------------------------------------------------------------------------

    AudioSourceStatus fillInput();


    // ------------------------------------------------------------------------
    // Decode
    // ------------------------------------------------------------------------

    bool decodeNextFrame();


    // ------------------------------------------------------------------------
    // Output
    // ------------------------------------------------------------------------

    bool outputNextSample(
        int16_t &left,
        int16_t &right
    );


    // ------------------------------------------------------------------------
    // Audio blocks
    // ------------------------------------------------------------------------

    audio_block_t *block_left;

    audio_block_t *block_right;

    uint16_t block_offset;


    void releaseBlocks();

    void finishPlayback();
};
