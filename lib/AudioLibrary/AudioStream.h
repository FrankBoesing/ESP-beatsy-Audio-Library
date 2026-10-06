#pragma once
#ifndef AudioStream_h
#define AudioStream_h

#include <Arduino.h>
#include <stdint.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include <esp_attr.h>
#include "common.h"


// -----------------------------------------------------------------------------
// Audio block
// -----------------------------------------------------------------------------

typedef struct audio_block_struct {
    uint8_t ref_count;
    uint8_t reserved1;
    uint16_t memory_pool_index;
    int16_t data[AUDIO_BLOCK_SAMPLES];
} audio_block_t;

// -----------------------------------------------------------------------------
// Forward declarations
// -----------------------------------------------------------------------------

class AudioStream;
class AudioConnection;

// -----------------------------------------------------------------------------
// AudioConnection
// -----------------------------------------------------------------------------

class AudioConnection {
  public:
    AudioConnection();

    AudioConnection(AudioStream &source, AudioStream &destination)
        : AudioConnection() {
        connect(source, destination);
    }

    AudioConnection(AudioStream &source, unsigned char sourceOutput,
                    AudioStream &destination, unsigned char destinationInput)
        : AudioConnection() {
        connect(source, sourceOutput, destination, destinationInput);
    }

    ~AudioConnection();

    int disconnect(void);
    int connect(void);

    int connect(AudioStream &source, AudioStream &destination) {
        return connect(source, 0, destination, 0);
    }

    int connect(AudioStream &source, unsigned char sourceOutput,
                AudioStream &destination, unsigned char destinationInput);

    friend class AudioStream;

  protected:
    AudioStream *src;
    AudioStream *dst;

    unsigned char src_index;
    unsigned char dest_index;

    AudioConnection *next_dest;

    bool isConnected;
};

// -----------------------------------------------------------------------------
// Audio memory
//
// The mask is part of the AudioMemory() allocation and therefore has the same
// lifetime as the audio block pool.
//
// Only one active pool is supported, matching the original AudioStream model.
// -----------------------------------------------------------------------------

#define AUDIO_MEMORY_MASK_WORDS(num) (((num) + 31U) / 32U)

#define AudioMemory(num)                                                       \
    do {                                                                       \
        static audio_block_t data[num];                                        \
        static uint32_t audio_memory_masks[AUDIO_MEMORY_MASK_WORDS(num)];      \
        AudioStream::initialize_memory(data, (num), audio_memory_masks,        \
                                       AUDIO_MEMORY_MASK_WORDS(num));           \
    } while (0)

// -----------------------------------------------------------------------------
// AudioStream
// -----------------------------------------------------------------------------

class AudioStream {
  public:
    AudioStream(unsigned char ninput, audio_block_t **iqueue);

    virtual ~AudioStream() = default;

    // -------------------------------------------------------------------------
    // Memory pool
    // -------------------------------------------------------------------------

    static void initialize_memory(audio_block_t *data, unsigned int num,
                                  uint32_t *available_mask,
                                  unsigned int mask_words);

    // -------------------------------------------------------------------------
    // Diagnostics
    // -------------------------------------------------------------------------

    float AudioProcessorUsage(void) const;
    float AudioProcessorUsageMax(void) const;
    void AudioProcessorUsageMaxReset(void);
    static float AudioProcessorUsageTotal(void);
    static float AudioProcessorUsageTotalMax(void);
    static void AudioProcessorUsageTotalMaxReset(void);
    static float processorUsage(uint8_t core);

    static uint16_t AudioMemoryUsage(void);
    static uint16_t AudioMemoryUsageMax(void);
    static void AudioMemoryUsageMaxReset(void);

    bool isActive(void) const {
        return active;
    }

    // Per-stream processor statistics.
    //
    // These remain part of the existing interface and are updated after the
    // realtime graph traversal.
    uint32_t cpu_time_us = 0;
    uint32_t cpu_time_max_us = 0;

    // Global processor statistics.
    static uint32_t cpu_time_total_us;
    static uint32_t cpu_time_total_max_us;

  protected:
    // -------------------------------------------------------------------------
    // Audio block ownership
    // -------------------------------------------------------------------------

    static audio_block_t *allocate(void);
    static void release(audio_block_t *block);

    void transmit(audio_block_t *block, unsigned char index = 0);

    audio_block_t *receiveReadOnly(unsigned int index = 0);
    audio_block_t *receiveWritable(unsigned int index = 0);

    // -------------------------------------------------------------------------
    // Audio timing / scheduler
    // -------------------------------------------------------------------------

    // Software-clocked sample rate. Hardware audio sources will be able to
    // take over the update clock later without changing the AudioStream API.
    static bool setSampleRate(float rate);
    static float sampleRate(void);
    static uint32_t blockPeriodUs(void);

    // Initializes the scheduler infrastructure. The current implementation
    // uses a FreeRTOS task notification as the hand-off from the clock source
    // to the audio processing task.
    static bool update_setup(void);
    static void update_stop(void);
    static bool setExternalUpdateClock(bool enabled);

    // Signal one audio update cycle. This function is intentionally kept as
    // the common entry point for timer and future I2S/DMA clock sources.
    static void update_all(void);
    static bool IRAM_ATTR update_all_from_isr(void);
    static void disableUpdates(void);
    static void enableUpdates(void);

    // Used by the current test infrastructure for deterministic synchronous
    // execution. The real scheduler uses update_all().
    static void process_all_now(void);

    virtual bool beginHardware() {
        return true;
    }

    friend class AudioConnection;
    friend void AudioInterrupts();
    friend void AudioNoInterrupts();

    // -------------------------------------------------------------------------
    // Stream information
    // -------------------------------------------------------------------------

    bool active;

    unsigned char num_inputs;

    uint8_t numConnections;

    virtual void update(void) = 0;

  private:
    // -------------------------------------------------------------------------
    // Memory pool
    // -------------------------------------------------------------------------

    static audio_block_t *memory_pool;
    static uint16_t memory_pool_size;

    static uint32_t *memory_pool_available_mask;
    static uint16_t memory_pool_mask_words;

    static uint16_t memory_pool_first_mask;

    static uint16_t memory_used;
    static uint16_t memory_used_max;

    // -------------------------------------------------------------------------
    // Graph
    // -------------------------------------------------------------------------

    AudioConnection *destination_list;
    audio_block_t **inputQueue;
    static AudioStream *first_update;
    AudioStream *next_update;

    // -------------------------------------------------------------------------
    // Internal processor timing
    //
    // Raw cycle counts are collected during realtime processing. Conversion
    // to microseconds is intentionally performed after the graph traversal.
    // -------------------------------------------------------------------------

    uint32_t cpu_time_cycles = 0;
    uint32_t cpu_time_max_cycles = 0;

    // -------------------------------------------------------------------------
    // Internal helpers
    // These functions require the audio critical section to be held.
    // -------------------------------------------------------------------------

    static audio_block_t *allocate_locked(void);
    static void release_locked(audio_block_t *block);
    static bool is_block_from_pool(const audio_block_t *block);

    // -------------------------------------------------------------------------
    // ESP32 synchronization
    // -------------------------------------------------------------------------

    static portMUX_TYPE audio_mux;

    // -------------------------------------------------------------------------
    // Scheduler internals
    // -------------------------------------------------------------------------

    static void scheduler_task(void *parameter);
    static void software_timer_callback(void *parameter);

    static TaskHandle_t audio_task_handle;
    static SemaphoreHandle_t audio_update_mutex;
    static esp_timer_handle_t software_timer;
    static bool update_scheduled;
    static bool external_update_clock;
    static float audio_sample_rate;
};


inline float AudioProcessorUsage() {
    return AudioStream::AudioProcessorUsageTotal();
}

inline float AudioProcessorUsageMax() {
    return AudioStream::AudioProcessorUsageTotalMax();
}

inline void AudioProcessorUsageMaxReset() {
    AudioStream::AudioProcessorUsageTotalMaxReset();
}

inline uint16_t AudioMemoryUsage() {
    return AudioStream::AudioMemoryUsage();
}

inline uint16_t AudioMemoryUsageMax() {
    return AudioStream::AudioMemoryUsageMax();
}

inline void AudioMemoryUsageMaxReset() {
    AudioStream::AudioMemoryUsageMaxReset();
}

// Pair these from task context; nested calls are supported.
inline void AudioInterrupts() {
    AudioStream::enableUpdates();
}

inline void AudioNoInterrupts() {
    AudioStream::disableUpdates();
}

#endif // AudioStream_h
