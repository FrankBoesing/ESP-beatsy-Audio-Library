#include <Arduino.h>
#include "AudioStream.h"
#include <math.h>
#include "defines.h"

// =============================================================================
// Static members
// =============================================================================

audio_block_t *AudioStream::memory_pool = nullptr;

uint16_t AudioStream::memory_pool_size = 0;

uint32_t *AudioStream::memory_pool_available_mask = nullptr;
uint16_t AudioStream::memory_pool_mask_words = 0;

uint16_t AudioStream::memory_pool_first_mask = 0;

uint16_t AudioStream::memory_used = 0;
uint16_t AudioStream::memory_used_max = 0;

uint32_t AudioStream::cpu_time_total_us = 0;
uint32_t AudioStream::cpu_time_total_max_us = 0;

AudioStream *AudioStream::first_update = nullptr;

portMUX_TYPE AudioStream::audio_mux = portMUX_INITIALIZER_UNLOCKED;

TaskHandle_t AudioStream::audio_task_handle = nullptr;
esp_timer_handle_t AudioStream::software_timer = nullptr;
bool AudioStream::update_scheduled = false;
bool AudioStream::external_update_clock = false;
float AudioStream::audio_sample_rate = AUDIO_SAMPLE_RATE_EXACT;

// =============================================================================
// AudioStream constructor
// =============================================================================

OSIZE
AudioStream::AudioStream(unsigned char ninput, audio_block_t **iqueue)
    : active(false), num_inputs(ninput), numConnections(0),
      destination_list(nullptr), inputQueue(iqueue), next_update(nullptr) {
    for (unsigned int i = 0; i < num_inputs; ++i) {
        inputQueue[i] = nullptr;
    }

    // Add stream to update list.
    //
    // This mirrors the original AudioStream implementation. Stream objects
    // are normally constructed during initialization before normal audio
    // processing starts.

    portENTER_CRITICAL(&audio_mux);

    if (first_update == nullptr) {
        first_update = this;
    } else {
        AudioStream *p = first_update;

        while (p->next_update != nullptr) {
            p = p->next_update;
        }

        p->next_update = this;
    }

    portEXIT_CRITICAL(&audio_mux);
}

// =============================================================================
// Audio timing / scheduler
// =============================================================================
OSIZE
bool AudioStream::setSampleRate(float rate) {
    if (!(rate > 0.0f) || !isfinite(rate)) {
        return false;
    }

    bool use_software_clock;
    portENTER_CRITICAL(&audio_mux);
    audio_sample_rate = rate;
    use_software_clock = !external_update_clock;
    portEXIT_CRITICAL(&audio_mux);

    // The software clock is currently the only clock source. Re-arm it with
    // the new block period if the scheduler is already running.
    if (software_timer != nullptr && use_software_clock) {
        const uint64_t period = blockPeriodUs();
        if (period == 0) {
            return false;
        }
        esp_timer_stop(software_timer);
        if (esp_timer_start_periodic(software_timer, period) != ESP_OK) {
            return false;
        }
    }

    return true;
}

OSIZE
float AudioStream::sampleRate(void) {
    portENTER_CRITICAL(&audio_mux);
    const float rate = audio_sample_rate;
    portEXIT_CRITICAL(&audio_mux);
    return rate;
}

OSIZE
uint32_t AudioStream::blockPeriodUs(void) {
    const float rate = sampleRate();
    if (!(rate > 0.0f) || !isfinite(rate)) {
        return 0;
    }

    const double period =
        (1000000.0 * static_cast<double>(AUDIO_BLOCK_SAMPLES)) /
        static_cast<double>(rate);

    if (period < 1.0 || period > 0xFFFFFFFFu) {
        return 0;
    }

    return static_cast<uint32_t>(period + 0.5);
}

OSIZE
bool AudioStream::setExternalUpdateClock(bool enabled) {
    portENTER_CRITICAL(&audio_mux);
    const bool already_enabled = external_update_clock;
    const bool scheduler_running = update_scheduled;
    portEXIT_CRITICAL(&audio_mux);

    if (already_enabled == enabled) {
        return true;
    }
    if (!scheduler_running || software_timer == nullptr) {
        return false;
    }

    if (enabled) {
        if (esp_timer_stop(software_timer) != ESP_OK) {
            return false;
        }
    } else {
        const uint32_t period = blockPeriodUs();
        if (period == 0 ||
            esp_timer_start_periodic(software_timer, period) != ESP_OK) {
            return false;
        }
    }

    portENTER_CRITICAL(&audio_mux);
    external_update_clock = enabled;
    portEXIT_CRITICAL(&audio_mux);
    return true;
}

OSIZE
bool AudioStream::update_setup(void) {
    portENTER_CRITICAL(&audio_mux);
    if (update_scheduled) {
        portEXIT_CRITICAL(&audio_mux);
        return false;
    }
    portEXIT_CRITICAL(&audio_mux);

    if (audio_task_handle == nullptr) {
        BaseType_t result =
            xTaskCreate(scheduler_task, "AudioTask", 4096, nullptr,
                        configMAX_PRIORITIES - 2, &audio_task_handle);

        if (result != pdPASS) {
            audio_task_handle = nullptr;
            return false;
        }
    }

    esp_timer_create_args_t timer_args = {};
    timer_args.callback = &AudioStream::software_timer_callback;
    timer_args.arg = nullptr;
    timer_args.dispatch_method = ESP_TIMER_TASK;
    timer_args.name = "AudioClock";

    if (software_timer == nullptr) {
        if (esp_timer_create(&timer_args, &software_timer) != ESP_OK) {
            vTaskDelete(audio_task_handle);
            audio_task_handle = nullptr;
            return false;
        }
    }

    const uint32_t period = blockPeriodUs();
    if (period == 0 ||
        esp_timer_start_periodic(software_timer, period) != ESP_OK) {
        esp_timer_delete(software_timer);
        software_timer = nullptr;
        vTaskDelete(audio_task_handle);
        audio_task_handle = nullptr;
        return false;
    }

    portENTER_CRITICAL(&audio_mux);
    update_scheduled = true;
    external_update_clock = false;
    portEXIT_CRITICAL(&audio_mux);

    return true;
}

OSIZE
void AudioStream::update_stop(void) {
    portENTER_CRITICAL(&audio_mux);
    update_scheduled = false;
    external_update_clock = false;
    portEXIT_CRITICAL(&audio_mux);

    if (software_timer != nullptr) {
        esp_timer_stop(software_timer);
    }

    if (audio_task_handle != nullptr) {
        xTaskNotifyGive(audio_task_handle);
        vTaskDelay(1);
        vTaskDelete(audio_task_handle);
        audio_task_handle = nullptr;
    }

    if (software_timer != nullptr) {
        esp_timer_delete(software_timer);
        software_timer = nullptr;
    }
}

OSPEED
void AudioStream::software_timer_callback(void *) {
    update_all();
}

OSPEED
void AudioStream::update_all(void) {
    TaskHandle_t task = audio_task_handle;
    if (task != nullptr) {
        xTaskNotifyGive(task);
    }
}

OSPEED
bool IRAM_ATTR AudioStream::update_all_from_isr(void) {
    TaskHandle_t task = audio_task_handle;
    if (task != nullptr) {
        BaseType_t higher_priority_task_woken = pdFALSE;
        vTaskNotifyGiveFromISR(task, &higher_priority_task_woken);
        return higher_priority_task_woken == pdTRUE;
    }
    return false;
}

OSPEED
void AudioStream::scheduler_task(void *) {
    for (;;) {
        ulTaskNotifyTake(pdFALSE, portMAX_DELAY);

        bool running;
        portENTER_CRITICAL(&audio_mux);
        running = update_scheduled;
        portEXIT_CRITICAL(&audio_mux);

        if (running) {
            process_all_now();
        }
    }
}

OSPEED
void AudioStream::process_all_now(void) {
    const uint32_t total_start = micros();

    AudioStream *stream;
    portENTER_CRITICAL(&audio_mux);
    stream = first_update;
    portEXIT_CRITICAL(&audio_mux);

    while (stream != nullptr) {
        bool run_update;

        portENTER_CRITICAL(&audio_mux);
        run_update = stream->active;
        portEXIT_CRITICAL(&audio_mux);

        if (run_update) {
            const uint32_t start = micros();
            stream->update();
            const uint32_t elapsed = micros() - start;

            stream->cpu_time_us = elapsed;
            if (elapsed > stream->cpu_time_max_us) {
                stream->cpu_time_max_us = elapsed;
            }
        }

        portENTER_CRITICAL(&audio_mux);
        stream = stream->next_update;
        portEXIT_CRITICAL(&audio_mux);
    }

    const uint32_t total_elapsed = micros() - total_start;

    portENTER_CRITICAL(&audio_mux);
    cpu_time_total_us = total_elapsed;
    if (total_elapsed > cpu_time_total_max_us) {
        cpu_time_total_max_us = total_elapsed;
    }
    portEXIT_CRITICAL(&audio_mux);
}

// =============================================================================
// Audio memory initialization
// =============================================================================
OSIZE
void AudioStream::initialize_memory(audio_block_t *data, unsigned int num,
                                    uint32_t *available_mask,
                                    unsigned int mask_words) {
    if (data == nullptr || available_mask == nullptr || num == 0 ||
        mask_words == 0) {
        return;
    }

    portENTER_CRITICAL(&audio_mux);

    memory_pool = data;
    memory_pool_size = static_cast<uint16_t>(num);

    memory_pool_available_mask = available_mask;
    memory_pool_mask_words = static_cast<uint16_t>(mask_words);

    memory_pool_first_mask = 0;

    memory_used = 0;
    memory_used_max = 0;

    // Clear all mask words first.
    for (unsigned int i = 0; i < mask_words; ++i) {
        available_mask[i] = 0;
    }

    // Mark all blocks as available.
    for (unsigned int i = 0; i < num; ++i) {
        available_mask[i >> 5] |= (uint32_t(1) << (i & 0x1F));

        data[i].memory_pool_index = static_cast<uint16_t>(i);

        data[i].ref_count = 0;
        data[i].reserved1 = 0;
    }

    portEXIT_CRITICAL(&audio_mux);

    // As in the Teensy implementation, initialize_memory() provides the
    // software-clock fallback when no hardware audio clock exists yet.
    if (audio_task_handle == nullptr) {
        update_setup();
    }

    /* Hardware der Audio-Streams initialisieren */
    for (AudioStream *p = first_update; p != nullptr; p = p->next_update) {
        if (!p->beginHardware()) {
            // Fehler behandeln
            ESP_LOGE("Audio", "Hardware initialization failed");
        }
    }
}

// =============================================================================
// Check whether a block belongs to our configured pool
// =============================================================================

bool AudioStream::is_block_from_pool(const audio_block_t *block) {
    if (block == nullptr || memory_pool == nullptr || memory_pool_size == 0) {
        return false;
    }

    uintptr_t block_address = reinterpret_cast<uintptr_t>(block);

    uintptr_t pool_address = reinterpret_cast<uintptr_t>(memory_pool);

    uintptr_t pool_end =
        pool_address + sizeof(audio_block_t) * memory_pool_size;

    if (block_address < pool_address || block_address >= pool_end) {
        return false;
    }

    uintptr_t offset = block_address - pool_address;

    return (offset % sizeof(audio_block_t)) == 0;
}

// =============================================================================
// Allocate block - internal
//
// The caller MUST hold audio_mux.
// =============================================================================

audio_block_t *AudioStream::allocate_locked(void) {
    if (memory_pool == nullptr || memory_pool_available_mask == nullptr ||
        memory_pool_mask_words == 0) {
        return nullptr;
    }

    uint16_t index = memory_pool_first_mask;

    while (index < memory_pool_mask_words) {

        uint32_t available = memory_pool_available_mask[index];

        if (available != 0) {

            // Find the lowest available bit.
            uint32_t bit = available & (~available + 1U);

            uint32_t bit_index =
                static_cast<uint32_t>(__builtin_ctz(available));

            available &= ~bit;

            memory_pool_available_mask[index] = available;

            if (available == 0) {
                memory_pool_first_mask = index + 1;
            } else {
                memory_pool_first_mask = index;
            }

            uint32_t block_index =
                (static_cast<uint32_t>(index) << 5) + bit_index;

            if (block_index >= memory_pool_size) {
                // This can only happen for unused bits in the final mask.
                // Put the bit back and continue searching.
                memory_pool_available_mask[index] |= bit;
                ++index;
                memory_pool_first_mask = index;
                continue;
            }

            audio_block_t *block = &memory_pool[block_index];

            block->ref_count = 1;

            ++memory_used;

            if (memory_used > memory_used_max) {
                memory_used_max = memory_used;
            }

            return block;
        }

        ++index;
        memory_pool_first_mask = index;
    }

    return nullptr;
}

// =============================================================================
// Allocate block
// =============================================================================

audio_block_t *AudioStream::allocate(void) {
    portENTER_CRITICAL(&audio_mux);

    audio_block_t *block = allocate_locked();

    portEXIT_CRITICAL(&audio_mux);

    return block;
}

// =============================================================================
// Release block - internal
//
// The caller MUST hold audio_mux.
// =============================================================================

void AudioStream::release_locked(audio_block_t *block) {
    if (block == nullptr) {
        return;
    }

    if (!is_block_from_pool(block)) {
        return;
    }

    if (block->ref_count == 0) {
        // Invalid ownership state. Do not modify the pool.
        return;
    }

    if (block->ref_count > 1) {
        --block->ref_count;
        return;
    }

    // ref_count == 1:
    // The block is now returned to the pool.

    uint16_t block_index = block->memory_pool_index;

    if (block_index >= memory_pool_size) {
        return;
    }

    uint16_t mask_index = block_index >> 5;

    uint32_t bit = uint32_t(1) << (block_index & 0x1F);

    memory_pool_available_mask[mask_index] |= bit;

    if (mask_index < memory_pool_first_mask) {
        memory_pool_first_mask = mask_index;
    }

    block->ref_count = 0;

    if (memory_used > 0) {
        --memory_used;
    }
}

// =============================================================================
// Release block
// =============================================================================

void AudioStream::release(audio_block_t *block) {
    portENTER_CRITICAL(&audio_mux);

    release_locked(block);

    portEXIT_CRITICAL(&audio_mux);
}

// =============================================================================
// Transmit
// =============================================================================
OSPEED
void AudioStream::transmit(audio_block_t *block, unsigned char index) {
    if (block == nullptr) {
        return;
    }

    portENTER_CRITICAL(&audio_mux);

    for (AudioConnection *c = destination_list; c != nullptr;
         c = c->next_dest) {

        if (c->src_index != index) {
            continue;
        }

        if (c->dst == nullptr) {
            continue;
        }

        if (c->dest_index >= c->dst->num_inputs) {
            continue;
        }

        if (c->dst->inputQueue[c->dest_index] == nullptr) {

            c->dst->inputQueue[c->dest_index] = block;

            ++block->ref_count;
        }
    }

    portEXIT_CRITICAL(&audio_mux);
}

// =============================================================================
// Receive read-only
// =============================================================================
OSPEED
audio_block_t *AudioStream::receiveReadOnly(unsigned int index) {
    if (index >= num_inputs) {
        return nullptr;
    }

    portENTER_CRITICAL(&audio_mux);

    audio_block_t *block = inputQueue[index];

    inputQueue[index] = nullptr;

    portEXIT_CRITICAL(&audio_mux);

    return block;
}

// =============================================================================
// Receive writable
//
// If the block is shared, perform copy-on-write.
// =============================================================================
OSPEED
audio_block_t *AudioStream::receiveWritable(unsigned int index) {
    if (index >= num_inputs) {
        return nullptr;
    }

    portENTER_CRITICAL(&audio_mux);

    audio_block_t *input = inputQueue[index];

    inputQueue[index] = nullptr;

    if (input == nullptr) {
        portEXIT_CRITICAL(&audio_mux);
        return nullptr;
    }

    if (input->ref_count <= 1) {
        // We are the sole owner.
        portEXIT_CRITICAL(&audio_mux);
        return input;
    }

    // Shared block: create a private copy.

    audio_block_t *copy = allocate_locked();

    if (copy != nullptr) {

        memcpy(copy->data, input->data, sizeof(copy->data));

        // We no longer own the input block.
        release_locked(input);

        portEXIT_CRITICAL(&audio_mux);

        return copy;
    }

    // Allocation failed.
    //
    // We cannot return the shared block as writable because that would
    // violate the ownership contract.
    //
    // Give up our reference and report failure.

    release_locked(input);

    portEXIT_CRITICAL(&audio_mux);

    return nullptr;
}

// =============================================================================
// Processor usage
// =============================================================================
OSPEED
float AudioStream::processorUsage(void) const {
    const float period = static_cast<float>(blockPeriodUs());
    if (period <= 0.0f)
        return 0.0f;
    return (static_cast<float>(cpu_time_us) * 100.0f) / period;
}

OSPEED
float AudioStream::processorUsageMax(void) const {
    const float period = static_cast<float>(blockPeriodUs());
    if (period <= 0.0f)
        return 0.0f;
    return (static_cast<float>(cpu_time_max_us) * 100.0f) / period;
}

OSIZE
void AudioStream::processorUsageMaxReset(void) {
    portENTER_CRITICAL(&audio_mux);
    cpu_time_max_us = cpu_time_us;
    portEXIT_CRITICAL(&audio_mux);
}

// =============================================================================
// Memory usage
// =============================================================================

uint16_t AudioStream::memoryUsage(void) {
    portENTER_CRITICAL(&audio_mux);

    uint16_t value = memory_used;

    portEXIT_CRITICAL(&audio_mux);

    return value;
}

uint16_t AudioStream::memoryUsageMax(void) {
    portENTER_CRITICAL(&audio_mux);

    uint16_t value = memory_used_max;

    portEXIT_CRITICAL(&audio_mux);

    return value;
}

void AudioStream::memoryUsageMaxReset(void) {
    portENTER_CRITICAL(&audio_mux);

    memory_used_max = memory_used;

    portEXIT_CRITICAL(&audio_mux);
}

// =============================================================================
// AudioConnection constructor
// =============================================================================

AudioConnection::AudioConnection()
    : src(nullptr), dst(nullptr), src_index(0), dest_index(0),
      next_dest(nullptr), isConnected(false) {}

// =============================================================================
// AudioConnection destructor
// =============================================================================

AudioConnection::~AudioConnection() {
    disconnect();
}

// =============================================================================
// connect()
// =============================================================================

int AudioConnection::connect(void) {
    if (isConnected) {
        return 1;
    }

    if (src == nullptr || dst == nullptr) {
        return 3;
    }

    if (dest_index >= dst->num_inputs) {
        return 2;
    }

    portENTER_CRITICAL(&AudioStream::audio_mux);

    // Check whether the destination input is already used.

    for (AudioStream *s = AudioStream::first_update; s != nullptr;
         s = s->next_update) {

        for (AudioConnection *p = s->destination_list; p != nullptr;
             p = p->next_dest) {

            if (p->dst == dst && p->dest_index == dest_index) {

                portEXIT_CRITICAL(&AudioStream::audio_mux);
                return 4;
            }
        }
    }

    // Insert into source destination list.

    AudioConnection *p = src->destination_list;

    if (p == nullptr) {

        src->destination_list = this;

    } else {

        while (p->next_dest != nullptr) {
            p = p->next_dest;
        }

        p->next_dest = this;
    }

    next_dest = nullptr;

    ++src->numConnections;
    src->active = true;

    ++dst->numConnections;
    dst->active = true;

    isConnected = true;

    portEXIT_CRITICAL(&AudioStream::audio_mux);

    return 0;
}

// =============================================================================
// connect(source, ...)
// =============================================================================

int AudioConnection::connect(AudioStream &source, unsigned char sourceOutput,
                             AudioStream &destination,
                             unsigned char destinationInput) {
    if (isConnected) {
        return 1;
    }

    src = &source;
    dst = &destination;

    src_index = sourceOutput;
    dest_index = destinationInput;

    return connect();
}

// =============================================================================
// disconnect()
// =============================================================================

int AudioConnection::disconnect(void) {
    if (!isConnected) {
        return 1;
    }

    if (src == nullptr || dst == nullptr) {
        return 2;
    }

    if (dest_index >= dst->num_inputs) {
        return 2;
    }

    portENTER_CRITICAL(&AudioStream::audio_mux);

    // Remove this connection from the source list.

    AudioConnection *p = src->destination_list;

    if (p == nullptr) {

        portEXIT_CRITICAL(&AudioStream::audio_mux);
        return 3;

    } else if (p == this) {

        src->destination_list = next_dest;

    } else {

        while (p != nullptr) {

            if (p->next_dest == this) {
                p->next_dest = next_dest;
                break;
            }

            p = p->next_dest;
        }
    }

    // Release any block that is still queued at the destination.

    audio_block_t *pending = dst->inputQueue[dest_index];

    if (pending != nullptr) {

        dst->inputQueue[dest_index] = nullptr;

        AudioStream::release_locked(pending);
    }

    // Update active state.

    if (src->numConnections > 0) {
        --src->numConnections;
    }

    if (src->numConnections == 0) {
        src->active = false;
    }

    if (dst->numConnections > 0) {
        --dst->numConnections;
    }

    if (dst->numConnections == 0) {
        dst->active = false;
    }

    isConnected = false;
    next_dest = nullptr;

    portEXIT_CRITICAL(&AudioStream::audio_mux);

    return 0;
}
