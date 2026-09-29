#ifndef play_sd_wav_h_
#define play_sd_wav_h_

#include <Arduino.h>
#include <AudioStream.h>
#include "AudioSource.h"

#if defined(ARDUINO_ARCH_ESP32)
#include <FS.h>
#include <SD_MMC.h>
#endif


class AudioPlaySdWav : public AudioStream
{
public:
    AudioPlaySdWav()
        : AudioStream(0, nullptr),
          state(STOPPED),
          source(nullptr),
          own_source(false),
          block_left(nullptr),
          block_right(nullptr)
    {
        begin();
    }

    ~AudioPlaySdWav() override;

    void begin();

    // Bestehende API
    bool play(const char *filename);

#if defined(ARDUINO_ARCH_ESP32)
    // Beliebiges Arduino-ESP32 Filesystem
    bool play(fs::FS &fs, const char *filename);
#endif

    // Generische AudioSource
    bool play(AudioSource &source);

    void stop();
    void togglePlayPause();

    bool isPlaying();
    bool isPaused();
    bool isStopped();

    uint32_t positionMillis();
    uint32_t lengthMillis();

    void update() override;

private:

    enum State : uint8_t {
        STOPPED,
        PLAYING,
        PAUSED
    };

    State state;

    /*
     * Aktive AudioSource.
     */
    AudioSource *source;

    /*
     * true:
     *   Der Player hat die Source selbst erzeugt
     *   und ist für deren Lebensdauer verantwortlich.
     *
     * false:
     *   Die Source gehört dem Aufrufer.
     */
    bool own_source;

    uint64_t data_start;
    uint64_t data_length;
    uint64_t total_length;

    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint16_t bytes_per_sample;
    uint16_t block_align;

    uint32_t source_frame;
    uint32_t source_frames_read;

    audio_block_t *block_left;
    audio_block_t *block_right;
    uint16_t block_offset;

    // Source WAV samples are converted to signed 32-bit
    // internally before being reduced to the AudioStream format.
    int32_t previous_left;
    int32_t previous_right;
    int32_t next_left;
    int32_t next_right;

    bool have_previous;
    bool have_next;

    uint32_t resample_phase;
    uint32_t resample_step;

    /*
     * Gemeinsamer Startpfad für alle Sources.
     */
    bool startPlayback(
        AudioSource &source,
        bool takeOwnership
    );

    /*
     * Source-Verwaltung.
     */
    void closeSource();

    /*
     * Source I/O.
     */
    bool readExact(void *buffer, size_t length);
    bool seekAbsolute(uint64_t position);
    bool skip(uint64_t length);

    /*
     * WAV parsing.
     *
     * Diese Funktionen entsprechen funktional dem
     * bisherigen WAV-Parser.
     */
    bool parseWav();
    bool parseFmtChunk(uint64_t position, uint32_t size);
    bool findDataChunk();

    /*
     * PCM decoding.
     */
    bool readSourceFrame(
        int32_t &left,
        int32_t &right
    );

    int32_t decodeSample(
        const uint8_t *data
    ) const;

    bool getOutputSample(
        int16_t &left,
        int16_t &right
    );

    void releaseBlocks();
    void finishPlayback();
};

#endif
