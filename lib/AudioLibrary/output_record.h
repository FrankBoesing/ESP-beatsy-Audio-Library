#ifndef output_record_h_
#define output_record_h_

#include <Arduino.h>
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include "AudioStream.h"

class AudioOutputRecord : public AudioStream {
  public:
    static const size_t DEFAULT_BUFFER_SIZE = 512U * 1024U;
    static const int8_t CORE_UNPINNED = -1;

    AudioOutputRecord();
    ~AudioOutputRecord() override;

    // Must be called while no recording or writer task is active.
    // The size must hold at least one stereo audio block and be 4-byte aligned.
    bool setBufferSize(size_t bytes);
    size_t bufferSize() const { return _bufferSize; }

    // basePath is the filename without an extension, e.g. "/recording".
    // Creates /recording.tmp while recording and /recording.wav on success.
    // channels must be 1 (mono) or 2 (stereo).
    // writerCore: CORE_UNPINNED lets FreeRTOS choose; otherwise use core 0 or 1.
    bool begin(fs::FS &filesystem, const char *basePath, uint32_t sampleRate,
               uint8_t channels = 2, int8_t writerCore = CORE_UNPINNED);

    // Stops accepting audio, drains the ring buffer and finalizes/renames the
    // WAV file. Call from application/task context, not from update().
    // Returns true only if the WAV file was finalized and renamed successfully.
    bool stop();

    bool isRecording() const;
    bool hasError() const;

    void update() override;

  private:
    static const size_t WRITER_CHUNK_SIZE = 2048U;
    static const uint32_t WAV_SIZE_PLACEHOLDER = 0xFFFFFFFEUL;

    static void writerTaskEntry(void *parameter);
    void writerTaskLoop();

    bool writeInitialHeader();
    bool writeFinalHeader();
    bool writeBytes(const void *data, size_t size);
    bool writeFourCC(const char *id);
    bool writeLE16(uint16_t value);
    bool writeLE32(uint32_t value);

    void copyIntoRing(size_t index, const uint8_t *source, size_t size);
    void copyFromRing(size_t index, uint8_t *destination, size_t size);

    mutable portMUX_TYPE _bufferMux = portMUX_INITIALIZER_UNLOCKED;

    audio_block_t *inputQueueArray[2];

    fs::FS *_filesystem = nullptr;
    File _file;
    uint8_t *_buffer = nullptr;
    size_t _bufferSize = DEFAULT_BUFFER_SIZE;
    size_t _writeIndex = 0;
    size_t _readIndex = 0;
    size_t _bufferedBytes = 0;

    uint64_t _dataBytesAccepted = 0;
    uint64_t _dataBytesWritten = 0;
    uint64_t _maxDataBytes = 0;

    uint32_t _sampleRate = 0;
    uint32_t _dataSizeOffset = 0;
    uint32_t _dataOffset = 0;
    uint8_t _channels = 2;
    int8_t _writerCore = CORE_UNPINNED;

    char _tempPath[256] = {};
    char _finalPath[256] = {};

    SemaphoreHandle_t _writerWake = nullptr;
    SemaphoreHandle_t _writerDone = nullptr;

    bool _recording = false;
    bool _workerRunning = false;
    bool _completionPending = false;
    bool _stopRequested = false;
    bool _producerActive = false;
    bool _hasError = false;
    bool _lastResult = true;
};

#endif
