#pragma once
#ifndef AUDIO_DECODER_STREAM_H
#define AUDIO_DECODER_STREAM_H

#include "AudioStream.h"
#include <stddef.h>
#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/*
 * Generic decoder/output infrastructure shared by software codecs.
 *
 * Exactly two PCM buffers exist. The decoder task owns a buffer while it is
 * FILLING; AudioDecoderStream::update() owns a buffer while it is being read.
 *
 * Codec implementations only provide decodePcmBuffer().
 */
class AudioDecoderStream : public AudioStream {
  public:
    enum class DecodeResult : uint8_t { FILLED, RETRY, END_OF_STREAM, ERROR };

    explicit AudioDecoderStream(size_t pcmBufferSamples);
    ~AudioDecoderStream() override;

    AudioDecoderStream(const AudioDecoderStream &) = delete;
    AudioDecoderStream &operator=(const AudioDecoderStream &) = delete;

    /*
     * Creates the decoder task and the two PCM buffers.
     *
     * Deliberately NOT called by the constructor. Derived codec players call
     * this from play()/startPlayback().
     */
    bool startDecoderTask();

    /*
     * Stop path may wait for both the decoder task and an already running
     * realtime update(). No PCM memory is freed until both owners are gone.
     */
    void stopDecoderTask();

    bool decoderTaskRunning() const;
    bool decoderFinished() const;
    int decoderError() const;

    size_t pcmBufferSamples() const {
        return _pcmBufferSamples;
    }

    uint64_t samplesPlayed() const {
        return _samplesPlayed;
    }

    uint32_t pcmUnderrunFrames() const {
      return _pcmUnderrunFrames;
    }

  protected:
    enum : uint8_t { PCM_FREE = 0, PCM_FILLING, PCM_READY };

    /*
     * Called after normal playback completion, once the final PCM buffer has
     * been drained by update(). Derived players can use this to update their
     * public playback state independently of AudioStream::active.
     */
    virtual void onPlaybackFinished() {}

    /*
     * Fill one of the two PCM buffers.
     *
     * destination:
     *   exactly one of the two internally owned PCM buffers.
     *
     * capacity:
     *   maximum number of int16_t values that may be written.
     *
     * outSamples:
     *   number of valid int16_t values written.
     *
     * PCM data is always expected as stereo-interleaved:
     *   L, R, L, R, ...
     */
    virtual DecodeResult decodePcmBuffer(int16_t *destination, size_t capacity,
                                         size_t &outSamples) = 0;

    /*
     * Common realtime output implementation.
     *
     * It never waits for the decoder and always emits a complete
     * AudioStream block. Missing PCM is replaced with zeroes.
     */
    void update() override;

    void setDecoderFinished(int errorCode = 0);

  private:
    /*
     * AudioStream currently creates its realtime audio task at
     * configMAX_PRIORITIES - 2. The decoder therefore gets exactly one
     * priority level less.
     */
    static constexpr UBaseType_t DECODER_TASK_PRIORITY =
        configMAX_PRIORITIES - 3;

    static constexpr BaseType_t DECODER_TASK_CORE = AUDIO_DECODER_CORE;
    static constexpr uint16_t DECODER_TASK_STACK = 8192;
    static constexpr uint8_t PCM_BUFFER_COUNT = 2;

    static void decoderTaskEntry(void *arg);
    void decoderTaskLoop();

    bool allocatePcmBuffers();
    void freePcmBuffers();
    void clearDecoderState();

    int claimFreeBuffer();
    void releaseFilledBuffer(uint8_t index);
    void publishReadyBuffer(uint8_t index, size_t samples);

    int acquireReadyBuffer();
    void releaseReadBuffer(uint8_t index);

    size_t _pcmBufferSamples;

    int16_t *_pcm[PCM_BUFFER_COUNT] = {};
    volatile uint8_t _pcmState[PCM_BUFFER_COUNT] = {PCM_FREE, PCM_FREE};
    volatile size_t _pcmSamples[PCM_BUFFER_COUNT] = {0, 0};

    /*
     * Only the realtime audio side may hold a buffer as _readBuffer.
     * While held, the decoder can see it only as PCM_READY and therefore
     * cannot write it.
     */
    int8_t _readBuffer = -1;
    size_t _readPosition = 0;

    volatile bool _decoderStopRequested = false;
    volatile bool _decoderTaskRunning = false;
    volatile bool _decoderFinished = false;
    volatile int _decoderError = 0;

    /*
     * Protected by _decoderMux. stopDecoderTask() waits for this to become
     * false before freeing _pcm[].
     */
    volatile bool _updateRunning = false;

    uint64_t _samplesPlayed = 0;
    volatile uint32_t _pcmUnderrunFrames = 0;

    portMUX_TYPE _decoderMux = portMUX_INITIALIZER_UNLOCKED;

    TaskHandle_t _decoderTask = nullptr;
};

#endif
