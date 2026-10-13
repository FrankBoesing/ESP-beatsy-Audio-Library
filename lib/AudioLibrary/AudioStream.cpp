#include <Arduino.h>
#include "AudioStream.h"
#include <esp_cpu.h>
#include <esp_err.h>
#include <math.h>
#include <freertos/idf_additions.h>

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
SemaphoreHandle_t AudioStream::audio_update_mutex = nullptr;
esp_timer_handle_t AudioStream::software_timer = nullptr;
bool AudioStream::software_clock_deferred = false;
bool AudioStream::update_scheduled = false;
bool AudioStream::external_update_clock = false;
float AudioStream::audio_sample_rate = AUDIO_SAMPLE_RATE_EXACT;

// Conversion factor for the per-stream cycle counters collected in AudioTask.
static uint32_t cpu_cycles_per_us = 0;

float AudioStream::processorUsage(uint8_t core) {
// ESP Timer runtime statistics use microseconds, matching esp_timer_get_time().
// CPU-clock runtime statistics count cycles and are frequency-dependent, so the
// wall-time ratio below would be incorrect for that configuration.
#if defined(CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS) && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS && \
    defined(CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER) && CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER && \
    (!defined(CONFIG_FREERTOS_SMP) || !CONFIG_FREERTOS_SMP)
    if (core >= portNUM_PROCESSORS) {
        return -1.0f;
    }

    static configRUN_TIME_COUNTER_TYPE previousIdleRuntime[portNUM_PROCESSORS] = {};
    static uint64_t previousSampleUs[portNUM_PROCESSORS] = {};
    static bool initialized[portNUM_PROCESSORS] = {};

    TaskHandle_t idleTask = xTaskGetIdleTaskHandleForCore(core);
    if (idleTask == nullptr) {
        return -1.0f;
    }

    TaskStatus_t idleTaskStatus = {};
    vTaskGetInfo(idleTask, &idleTaskStatus, pdFALSE, eInvalid);

    const uint64_t nowUs = esp_timer_get_time();
    const configRUN_TIME_COUNTER_TYPE idleRuntime = idleTaskStatus.ulRunTimeCounter;

    if (!initialized[core]) {
        previousIdleRuntime[core] = idleRuntime;
        previousSampleUs[core] = nowUs;
        initialized[core] = true;
        return -1.0f;
    }

    const configRUN_TIME_COUNTER_TYPE idleDelta = idleRuntime - previousIdleRuntime[core];
    const uint64_t elapsedUs = nowUs - previousSampleUs[core];

    previousIdleRuntime[core] = idleRuntime;
    previousSampleUs[core] = nowUs;

    if (elapsedUs == 0) {
        return -1.0f;
    }

    const float idlePercent = (float)(idleDelta) * 100.0f / (float)(elapsedUs);

    return 100.0f - fminf(idlePercent, 100.0f);
#else
    (void)core;
    return -1.0f;
#endif
}

// =============================================================================
// AudioStream constructor
// =============================================================================

OSIZE
AudioStream::AudioStream(unsigned char ninput, audio_block_t **iqueue)
    : active(false), num_inputs(ninput), numConnections(0), destination_list(nullptr), inputQueue(iqueue),
      next_update(nullptr) {
    for (unsigned int i = 0; i < num_inputs; ++i) {
        inputQueue[i] = nullptr;
    }

    // Add stream to update list.
    //
    // The update mutex protects the list while the audio scheduler is running.
    // Before the scheduler exists, disableUpdates() is a no-op, so construction
    // still works during normal static initialization.
    disableUpdates();

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

    enableUpdates();
}

// =============================================================================
// AudioStream destructor
// =============================================================================
#ifdef AUDIOSTREAM_ENABLE_DYNAMIC_LIFETIME
OSIZE
AudioStream::~AudioStream() {
    // Graph changes are serialized through the same mutex used by the audio
    // processing task. The mutex is recursive, so this is also safe when the
    // caller already used AudioNoInterrupts().
    //
    // Do not destroy an AudioStream from its own update() callback. There is no
    // useful way for a task to suspend itself until the current member function
    // has returned, and the scheduler could otherwise continue with a dangling
    // stream pointer.
    disableUpdates();

    // Disconnect every connection whose source or destination is this stream.
    // We intentionally keep the existing graph representation and do a linear
    // search here. Object lifetime changes are control-plane operations and are
    // not part of the realtime path.
    for (;;) {
        AudioConnection *connection = nullptr;

        for (AudioStream *stream = first_update; stream != nullptr && connection == nullptr;
             stream = stream->next_update) {
            for (AudioConnection *p = stream->destination_list; p != nullptr; p = p->next_dest) {
                if (p->isConnected && (p->src == this || p->dst == this)) {
                    connection = p;
                    break;
                }
            }
        }

        if (connection == nullptr) {
            break;
        }

        connection->disconnect_locked();

        // The connection object may outlive the stream. Do not leave a dangling
        // endpoint pointer behind. A later connection destructor can then safely
        // call disconnect(), and the connection can be reused with new endpoints.
        connection->src = nullptr;
        connection->dst = nullptr;
    }

    // Release any block which is still queued at an input. Normally all input
    // queues belonging to connections have already been cleared above, but
    // clearing the whole array makes the object lifetime boundary explicit and
    // robust against orphaned queue entries.
    portENTER_CRITICAL(&audio_mux);

    for (unsigned int i = 0; i < num_inputs; ++i) {
        if (inputQueue[i] != nullptr) {
            release_locked(inputQueue[i]);
            inputQueue[i] = nullptr;
        }
    }

    // Remove the stream from the scheduler update list.
    if (first_update == this) {
        first_update = next_update;
    } else {
        AudioStream *p = first_update;
        while (p != nullptr && p->next_update != this) {
            p = p->next_update;
        }

        if (p != nullptr) {
            p->next_update = next_update;
        }
    }

    next_update = nullptr;
    destination_list = nullptr;
    active = false;
    numConnections = 0;

    portEXIT_CRITICAL(&audio_mux);

    enableUpdates();
}
#endif

// =============================================================================
// Audio timing / scheduler
// =============================================================================
OSIZE
bool AudioStream::setSampleRate(float rate) {
    if (!isfinite(rate) || rate <= 0.0f) {
        return false;
    }

    // Serialize clock reconfiguration with clock-source changes. This is a
    // control-plane operation, so waiting for the current graph traversal is
    // preferable to racing esp_timer_start/stop.
    const bool locked = audio_update_mutex != nullptr;
    if (locked && xSemaphoreTakeRecursive(audio_update_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    portENTER_CRITICAL(&audio_mux);
    audio_sample_rate = rate;
    const bool rearm_software_clock =
        update_scheduled && software_timer != nullptr && !external_update_clock &&
        !software_clock_deferred;
    esp_timer_handle_t timer = software_timer;
    portEXIT_CRITICAL(&audio_mux);

    bool ok = true;
    if (rearm_software_clock && timer != nullptr && esp_timer_is_active(timer)) {
        ok = stop_software_timer() && start_software_timer();
    }

    if (locked) {
        xSemaphoreGiveRecursive(audio_update_mutex);
    }
    return ok;
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

    const double period = (1000000.0 * (double)(AUDIO_BLOCK_SAMPLES)) / (double)(rate);

    if (period < 1.0 || period > 0xFFFFFFFFu) {
        return 0;
    }

    return (uint32_t)(period + 0.5);
}

OSIZE
bool AudioStream::setExternalUpdateClock(bool enabled) {
    // The clock source is selected once, during initialize_memory()'s
    // hardware-initialization pass. Later output start/stop calls do not switch
    // clocks; the audio graph and its timing source are expected to stay fixed.
    portENTER_CRITICAL(&audio_mux);
    const bool scheduler_running = update_scheduled;
    const bool timer_exists = software_timer != nullptr;
    const bool selecting_clock = software_clock_deferred;
    const bool current_clock = external_update_clock;
    portEXIT_CRITICAL(&audio_mux);

    if (!scheduler_running || !timer_exists) {
        return false;
    }

    if (!selecting_clock) {
        // After AudioMemory() the source is fixed. Accept idempotent requests,
        // but report a real source change as unsupported rather than pretending
        // a software-clock fallback was enabled when no such switch took place.
        return current_clock == enabled;
    }

    portENTER_CRITICAL(&audio_mux);
    external_update_clock = enabled;
    portEXIT_CRITICAL(&audio_mux);
    return true;
}

OSIZE
bool AudioStream::update_setup(void) {
    return update_setup_internal(false);
}

OSIZE
bool AudioStream::update_setup_internal(bool deferSoftwareClock) {
    portENTER_CRITICAL(&audio_mux);
    if (update_scheduled) {
        portEXIT_CRITICAL(&audio_mux);
        return false;
    }
    portEXIT_CRITICAL(&audio_mux);

    if (cpu_cycles_per_us == 0) {
        cpu_cycles_per_us = getCpuFrequencyMhz();
    }

    if (audio_update_mutex == nullptr) {
        audio_update_mutex = xSemaphoreCreateRecursiveMutex();
        if (audio_update_mutex == nullptr) {
            return false;
        }
    }

    if (audio_task_handle == nullptr) {
        BaseType_t result =
            xTaskCreatePinnedToCore(scheduler_task, "AudioTask", 4096, nullptr, configMAX_PRIORITIES - 2,
                                    &audio_task_handle, AUDIO_PROCESSING_CORE);

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

    // Publish scheduler state before allowing either clock source to notify
    // AudioTask. AudioMemory() defers the fallback until hardware init ends.
    portENTER_CRITICAL(&audio_mux);
    update_scheduled = true;
    external_update_clock = false;
    software_clock_deferred = deferSoftwareClock;
    portEXIT_CRITICAL(&audio_mux);

    if (!deferSoftwareClock && !start_software_timer()) {
        portENTER_CRITICAL(&audio_mux);
        update_scheduled = false;
        external_update_clock = false;
        software_clock_deferred = false;
        portEXIT_CRITICAL(&audio_mux);

        esp_timer_delete(software_timer);
        software_timer = nullptr;
        vTaskDelete(audio_task_handle);
        audio_task_handle = nullptr;
        return false;
    }

    return true;
}

OSIZE
bool AudioStream::start_software_timer(void) {
    const bool locked = audio_update_mutex != nullptr;
    if (locked && xSemaphoreTakeRecursive(audio_update_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    portENTER_CRITICAL(&audio_mux);
    const bool valid = update_scheduled && software_timer != nullptr;
    const bool shouldStart = valid && !external_update_clock && !software_clock_deferred;
    esp_timer_handle_t timer = software_timer;
    portEXIT_CRITICAL(&audio_mux);

    if (!valid) {
        if (locked) xSemaphoreGiveRecursive(audio_update_mutex);
        return false;
    }
    if (!shouldStart || esp_timer_is_active(timer)) {
        if (locked) xSemaphoreGiveRecursive(audio_update_mutex);
        return true;
    }

    const uint32_t period = blockPeriodUs();
    const esp_err_t err = period == 0 ? ESP_ERR_INVALID_ARG : esp_timer_start_periodic(timer, period);
    if (err != ESP_OK) {
        ESP_LOGE("Audio", "Could not start software audio clock: %s", esp_err_to_name(err));
    }

    if (locked) xSemaphoreGiveRecursive(audio_update_mutex);
    return err == ESP_OK;
}

OSIZE
bool AudioStream::stop_software_timer(void) {
    const bool locked = audio_update_mutex != nullptr;
    if (locked && xSemaphoreTakeRecursive(audio_update_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    portENTER_CRITICAL(&audio_mux);
    esp_timer_handle_t timer = software_timer;
    portEXIT_CRITICAL(&audio_mux);

    if (timer == nullptr || !esp_timer_is_active(timer)) {
        if (locked) xSemaphoreGiveRecursive(audio_update_mutex);
        return true;
    }

    const esp_err_t err = esp_timer_stop(timer);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE("Audio", "Could not stop software audio clock: %s", esp_err_to_name(err));
    }

    if (locked) xSemaphoreGiveRecursive(audio_update_mutex);
    return err == ESP_OK || err == ESP_ERR_INVALID_STATE;
}

OSIZE
void AudioStream::update_stop(void) {
    // Do not delete the scheduler from inside its own update() call.
    portENTER_CRITICAL(&audio_mux);
    TaskHandle_t task = audio_task_handle;
    SemaphoreHandle_t mutex = audio_update_mutex;
    portEXIT_CRITICAL(&audio_mux);

    if (task != nullptr && task == xTaskGetCurrentTaskHandle()) {
        ESP_LOGE("Audio", "update_stop() cannot be called from AudioTask");
        return;
    }

    // Taking the graph mutex waits for an in-flight process_all_now() to finish.
    // Once held, the scheduler cannot start another graph traversal. The caller
    // must stop/quiesce any external hardware clock before calling update_stop().
    const bool locked = mutex != nullptr;
    if (locked && xSemaphoreTakeRecursive(mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }

    portENTER_CRITICAL(&audio_mux);
    update_scheduled = false;
    external_update_clock = false;
    software_clock_deferred = false;
    task = audio_task_handle;
    esp_timer_handle_t timer = software_timer;
    portEXIT_CRITICAL(&audio_mux);

    // Stop and delete the software timer before deleting the task it can notify.
    if (timer != nullptr) {
        const esp_err_t stop_err = esp_timer_stop(timer);
        if (stop_err != ESP_OK && stop_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE("Audio", "Could not stop software audio clock: %s", esp_err_to_name(stop_err));
        }

        const esp_err_t delete_err = esp_timer_delete(timer);
        if (delete_err != ESP_OK) {
            ESP_LOGE("Audio", "Could not delete software audio clock: %s", esp_err_to_name(delete_err));
            if (locked) {
                xSemaphoreGiveRecursive(mutex);
            }
            return;
        }

        portENTER_CRITICAL(&audio_mux);
        if (software_timer == timer) {
            software_timer = nullptr;
        }
        portEXIT_CRITICAL(&audio_mux);
    }

    if (task != nullptr) {
        portENTER_CRITICAL(&audio_mux);
        if (audio_task_handle == task) {
            audio_task_handle = nullptr;
        }
        portEXIT_CRITICAL(&audio_mux);

        // The task either waits for a notification or is blocked trying to take
        // the mutex held by this function; no graph traversal is still running.
        vTaskDelete(task);
    }

    if (locked) {
        xSemaphoreGiveRecursive(mutex);
    }
}

OSPEED
void AudioStream::software_timer_callback(void *) {
    portENTER_CRITICAL(&audio_mux);
    const bool useSoftwareClock = update_scheduled && !software_clock_deferred && !external_update_clock;
    portEXIT_CRITICAL(&audio_mux);

    if (useSoftwareClock) {
        update_all();
    }
}

OSPEED
void AudioStream::update_all(void) {
    portENTER_CRITICAL(&audio_mux);
    TaskHandle_t task = update_scheduled ? audio_task_handle : nullptr;
    portEXIT_CRITICAL(&audio_mux);

    if (task != nullptr) {
        xTaskNotifyGive(task);
    }
}

OSIZE
void AudioStream::disableUpdates(void) {
    if (audio_update_mutex != nullptr) {
        xSemaphoreTakeRecursive(audio_update_mutex, portMAX_DELAY);
    }
}

OSIZE
void AudioStream::enableUpdates(void) {
    if (audio_update_mutex != nullptr && xSemaphoreGiveRecursive(audio_update_mutex) != pdTRUE) {
        ESP_LOGE("","AudioInterrupts called without a matching AudioNoInterrupts");
    }
}

OSPEED
bool IRAM_ATTR AudioStream::update_all_from_isr(void) {
    portENTER_CRITICAL_ISR(&audio_mux);
    TaskHandle_t task = update_scheduled ? audio_task_handle : nullptr;
    portEXIT_CRITICAL_ISR(&audio_mux);

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
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        process_all_now();
    }
}

OSPEED
void AudioStream::process_all_now(void) {
    if (audio_update_mutex == nullptr || xSemaphoreTakeRecursive(audio_update_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }

    // The recursive update mutex serializes the realtime graph traversal with
    // all graph modifications performed by connect(), disconnect(), stream
    // construction and stream destruction. No graph lock is needed inside the
    // realtime loop itself.
    portENTER_CRITICAL(&audio_mux);

    if (!update_scheduled) {
        portEXIT_CRITICAL(&audio_mux);
        xSemaphoreGiveRecursive(audio_update_mutex);
        return;
    }

    AudioStream *stream = first_update;

    portEXIT_CRITICAL(&audio_mux);

    // -------------------------------------------------------------------------
    // Realtime audio processing
    //
    // Only cycle counting is performed here. No division is needed.
    // -------------------------------------------------------------------------

    const uint32_t total_start = esp_cpu_get_cycle_count();

    while (stream != nullptr) {
        // Der komplette Audio-Graph muss nach update_setup() unveränderlich sein.
        if (stream->active) {
            const uint32_t start = esp_cpu_get_cycle_count();
            stream->update();
            const uint32_t elapsed_cycles = esp_cpu_get_cycle_count() - start;

            stream->cpu_time_cycles = elapsed_cycles;
            if (elapsed_cycles > stream->cpu_time_max_cycles) {
                stream->cpu_time_max_cycles = elapsed_cycles;
            }
        }

        stream = stream->next_update;
    }

    // -------------------------------------------------------------------------
    // Stop total timing BEFORE converting per-stream cycle counts.
    //
    // Therefore AudioProcessorUsageTotal() does not include the diagnostic
    // division overhead.
    // -------------------------------------------------------------------------

    const uint32_t total_elapsed_cycles = esp_cpu_get_cycle_count() - total_start;
    const uint32_t total_elapsed = cpu_cycles_per_us != 0 ? total_elapsed_cycles / cpu_cycles_per_us : 0;

    portENTER_CRITICAL(&audio_mux);

    cpu_time_total_us = total_elapsed;

    if (total_elapsed > cpu_time_total_max_us) {
        cpu_time_total_max_us = total_elapsed;
    }

    portEXIT_CRITICAL(&audio_mux);

    // -------------------------------------------------------------------------
    // Convert per-stream cycle counts to microseconds.
    //
    // This is deliberately outside the realtime graph traversal.
    // -------------------------------------------------------------------------

    if (cpu_cycles_per_us != 0) {
        for (stream = first_update; stream != nullptr; stream = stream->next_update) {
            if (stream->active) {
                stream->cpu_time_us = stream->cpu_time_cycles / cpu_cycles_per_us;

                stream->cpu_time_max_us = stream->cpu_time_max_cycles / cpu_cycles_per_us;
            }
        }
    }

    xSemaphoreGiveRecursive(audio_update_mutex);
}

// =============================================================================
// Audio memory initialization
// =============================================================================
OSIZE
void AudioStream::initialize_memory(audio_block_t *data, unsigned int num, uint32_t *available_mask,
                                    unsigned int mask_words) {
    if (data == nullptr || available_mask == nullptr || num == 0 || mask_words == 0) {
        return;
    }

    // Pause graph traversal while the pool and hardware clock source are set up.
    // On first initialization the mutex is created together with the scheduler,
    // so we take it immediately after the scheduler has been initialized.
    bool updates_locked = false;
    if (audio_update_mutex != nullptr) {
        disableUpdates();
        updates_locked = true;
    }

    portENTER_CRITICAL(&audio_mux);
    software_clock_deferred = true;
    portEXIT_CRITICAL(&audio_mux);

    if (audio_task_handle != nullptr && !stop_software_timer()) {
        portENTER_CRITICAL(&audio_mux);
        software_clock_deferred = false;
        portEXIT_CRITICAL(&audio_mux);
        if (updates_locked) {
            enableUpdates();
        }
        ESP_LOGE("Audio", "Could not pause software clock before memory initialization");
        return;
    }

    portENTER_CRITICAL(&audio_mux);

    memory_pool = data;
    memory_pool_size = (uint16_t)(num);
    memory_pool_available_mask = available_mask;
    memory_pool_mask_words = (uint16_t)(mask_words);
    memory_pool_first_mask = 0;
    memory_used = 0;
    memory_used_max = 0;

    for (unsigned int i = 0; i < mask_words; ++i) {
        available_mask[i] = 0xFFFFFFFFu;
    }

    const uint32_t remaining = num & 31;
    if (remaining != 0) {
        available_mask[mask_words - 1] = (uint32_t(1) << remaining) - 1u;
    }

    for (unsigned int i = 0; i < num; ++i) {
        data[i].memory_pool_index = (uint16_t)(i);
        data[i].ref_count = 0;
        data[i].reserved1 = 0;
    }

    portEXIT_CRITICAL(&audio_mux);

    // Create the scheduler if needed, but leave AudioClock stopped until all
    // input/output hardware has had its chance to claim the timing source.
    if (audio_task_handle == nullptr && !update_setup_internal(true)) {
        portENTER_CRITICAL(&audio_mux);
        software_clock_deferred = false;
        portEXIT_CRITICAL(&audio_mux);
        if (updates_locked) {
            enableUpdates();
        }
        ESP_LOGE("Audio", "Could not initialize audio scheduler");
        return;
    }

    if (!updates_locked && audio_update_mutex != nullptr) {
        disableUpdates();
        updates_locked = true;
    }

    for (AudioStream *p = first_update; p != nullptr; p = p->next_update) {
        if (!p->beginHardware()) {
            ESP_LOGE("Audio", "Hardware initialization failed");
        }
    }

    portENTER_CRITICAL(&audio_mux);
    software_clock_deferred = false;
    const bool useSoftwareClock = !external_update_clock;
    portEXIT_CRITICAL(&audio_mux);

    if (useSoftwareClock && !start_software_timer()) {
        ESP_LOGE("Audio", "Could not start software clock after hardware initialization");
    }

    if (updates_locked) {
        enableUpdates();
    }
}

// =============================================================================
// Check whether a block belongs to our configured pool
// =============================================================================

#ifdef AUDIOSTREAM_ENABLE_POOL_POINTER_CHECKS
bool AudioStream::is_block_from_pool(const audio_block_t *block) {
    if (block == nullptr || memory_pool == nullptr || memory_pool_size == 0) {
        return false;
    }

    const uintptr_t block_address = reinterpret_cast<uintptr_t>(block);
    const uintptr_t pool_address = reinterpret_cast<uintptr_t>(memory_pool);
    const uintptr_t pool_end = pool_address + sizeof(audio_block_t) * memory_pool_size;

    if (block_address < pool_address || block_address >= pool_end) {
        return false;
    }

    return ((block_address - pool_address) % sizeof(audio_block_t)) == 0;
}
#endif

// =============================================================================
// Allocate block - internal
//
// The caller MUST hold audio_mux.
// =============================================================================

audio_block_t *AudioStream::allocate_locked(void) {
    if (memory_pool == nullptr || memory_pool_available_mask == nullptr || memory_pool_mask_words == 0) {
        return nullptr;
    }

    uint16_t index = memory_pool_first_mask;

    while (index < memory_pool_mask_words) {
        uint32_t available = memory_pool_available_mask[index];

        if (available != 0) {
            // Find the lowest available bit.

            const uint32_t bit_index = __builtin_ctz(available);
            available &= available - 1u;

            memory_pool_available_mask[index] = available;

            if (available == 0) {
                memory_pool_first_mask = index + 1;
            } else {
                memory_pool_first_mask = index;
            }

            uint32_t block_index = ((uint32_t)(index) << 5) + bit_index;
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

#ifdef AUDIOSTREAM_ENABLE_POOL_POINTER_CHECKS
    if (!is_block_from_pool(block)) {
        return;
    }
#endif

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

    for (AudioConnection *c = destination_list; c != nullptr; c = c->next_dest) {
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
float AudioStream::AudioProcessorUsage(void) const {
    const float period = (float)(blockPeriodUs());
    if (period <= 0.0f) return 0.0f;
    return ((float)(cpu_time_us) * 100.0f) / period;
}

OSPEED
float AudioStream::AudioProcessorUsageMax(void) const {
    const float period = (float)(blockPeriodUs());
    if (period <= 0.0f) return 0.0f;
    return ((float)(cpu_time_max_us) * 100.0f) / period;
}

OSIZE
void AudioStream::AudioProcessorUsageMaxReset(void) {
    portENTER_CRITICAL(&audio_mux);

    cpu_time_max_cycles = cpu_time_cycles;
    cpu_time_max_us = cpu_time_us;

    portEXIT_CRITICAL(&audio_mux);
}

float AudioStream::AudioProcessorUsageTotal(void) {
    const uint32_t period = blockPeriodUs();
    if (period == 0) return 0.0f;

    portENTER_CRITICAL(&audio_mux);
    const uint32_t elapsed = cpu_time_total_us;
    portEXIT_CRITICAL(&audio_mux);

    return (float)(elapsed) * 100.0f / (float)(period);
}

float AudioStream::AudioProcessorUsageTotalMax(void) {
    const uint32_t period = blockPeriodUs();
    if (period == 0) return 0.0f;

    portENTER_CRITICAL(&audio_mux);
    const uint32_t elapsed = cpu_time_total_max_us;
    portEXIT_CRITICAL(&audio_mux);

    return (float)(elapsed) * 100.0f / (float)(period);
}

void AudioStream::AudioProcessorUsageTotalMaxReset(void) {
    portENTER_CRITICAL(&audio_mux);
    cpu_time_total_max_us = cpu_time_total_us;
    portEXIT_CRITICAL(&audio_mux);
}

// =============================================================================
// Memory usage
// =============================================================================

uint16_t AudioStream::AudioMemoryUsage(void) {
    portENTER_CRITICAL(&audio_mux);
    uint16_t value = memory_used;
    portEXIT_CRITICAL(&audio_mux);

    return value;
}

uint16_t AudioStream::AudioMemoryUsageMax(void) {
    portENTER_CRITICAL(&audio_mux);
    uint16_t value = memory_used_max;
    portEXIT_CRITICAL(&audio_mux);

    return value;
}

void AudioStream::AudioMemoryUsageMaxReset(void) {
    portENTER_CRITICAL(&audio_mux);
    memory_used_max = memory_used;
    portEXIT_CRITICAL(&audio_mux);
}

// =============================================================================
// AudioConnection constructor
// =============================================================================

AudioConnection::AudioConnection()
    : src(nullptr), dst(nullptr), src_index(0), dest_index(0), next_dest(nullptr), isConnected(false) {}

// =============================================================================
// AudioConnection destructor
// =============================================================================

AudioConnection::~AudioConnection() { disconnect(); }

// =============================================================================
// connect()
// =============================================================================

int AudioConnection::connect(void) {
    // Match the Teensy API model: a single connection operation is safe on its
    // own, while callers can wrap several graph changes in AudioNoInterrupts().
    AudioStream::disableUpdates();
    const int result = connect_locked();
    AudioStream::enableUpdates();
    return result;
}

int AudioConnection::connect_locked(void) {
    if (isConnected) {
        return 1;
    }

    if (src == nullptr || dst == nullptr) {
        return 3;
    }

    if (dest_index >= dst->num_inputs) {
        return 2;
    }

    // The caller holds the graph mutex, so the graph is stable while this
    // control-plane search runs. Keep the spinlock only around list mutation.
    for (AudioStream *s = AudioStream::first_update; s != nullptr; s = s->next_update) {
        for (AudioConnection *p = s->destination_list; p != nullptr; p = p->next_dest) {
            if (p->dst == dst && p->dest_index == dest_index) {
                return 4;
            }
        }
    }

    portENTER_CRITICAL(&AudioStream::audio_mux);

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

int AudioConnection::connect(AudioStream &source, unsigned char sourceOutput, AudioStream &destination,
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
    // A single disconnect is safe by itself. For several related changes, the
    // caller can hold AudioNoInterrupts() across all operations.
    AudioStream::disableUpdates();
    const int result = disconnect_locked();
    AudioStream::enableUpdates();
    return result;
}

int AudioConnection::disconnect_locked(void) {
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

    bool found = false;
    if (p == this) {
        src->destination_list = next_dest;
        found = true;
    } else {
        while (p != nullptr) {
            if (p->next_dest == this) {
                p->next_dest = next_dest;
                found = true;
                break;
            }

            p = p->next_dest;
        }
    }

    // Do not alter counters or destination queues if the connection was not
    // actually present in the source list.
    if (!found) {
        portEXIT_CRITICAL(&AudioStream::audio_mux);
        return 3;
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
