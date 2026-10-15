#include <Arduino.h>
#include "play_sd_wav.h"
#include "softcodecs/AudioSourceFile.h"

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>

static const char *TAG = "AudioPlaySdWav";
static portMUX_TYPE playback_rate_mux = portMUX_INITIALIZER_UNLOCKED;

namespace {

constexpr uint32_t RIFF_ID = 0x46464952UL; // "RIFF"
constexpr uint32_t WAVE_ID = 0x45564157UL; // "WAVE"
constexpr uint32_t FMT_ID = 0x20746D66UL;  // "fmt "
constexpr uint32_t DATA_ID = 0x61746164UL; // "data"

constexpr uint16_t WAV_PCM = 1;
constexpr uint16_t WAV_EXTENSIBLE = 0xFFFE;

static inline uint16_t readLE16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t readLE32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

void printChunk(uint32_t id, uint32_t size, uint32_t position) {
    #if CORE_DEBUG_LEVEL >= 5
    char name[5];

    name[0] = (char)(id & 0xFF);
    name[1] = (char)((id >> 8) & 0xFF);
    name[2] = (char)((id >> 16) & 0xFF);
    name[3] = (char)((id >> 24) & 0xFF);
    name[4] = 0;

    ESP_LOGV(TAG, "chunk '%s': position=%lu size=%lu", name,
             (unsigned long)position, (unsigned long)size);
    #endif
}
} // namespace

// ============================================================================
// Destructor
// ============================================================================

AudioPlaySdWav::~AudioPlaySdWav() {
    stop();
    closeSource();
}

// ============================================================================
// begin
// ============================================================================

void AudioPlaySdWav::begin() {
    state = STOPPED;

    /*
     * begin() darf die Source nicht blind schließen:
     * sie kann von außen verwaltet werden.
     */
    releaseBlocks();

    data_start = 0;
    data_length = 0;
    total_length = 0;

    sample_rate = 0;
    channels = 0;
    bits_per_sample = 0;
    bytes_per_sample = 0;
    block_align = 0;

    source_frame = 0;
    source_frames_read = 0;

    previous_left = 0;
    previous_right = 0;
    next_left = 0;
    next_right = 0;

    have_previous = false;
    have_next = false;

    source_phase = 0.0f;
    source_ended = false;
    current_playback_rate = target_playback_rate;
}

// ============================================================================
// Source ownership
// ============================================================================

void AudioPlaySdWav::closeSource() {
    if (source == nullptr) {
        own_source = false;
        return;
    }

    if (own_source) {
        source->close();
        delete source;
    }

    source = nullptr;
    own_source = false;
}

// ============================================================================
// Existing API: filename
// ============================================================================

bool AudioPlaySdWav::play(const char *filename) {
    return play(SD_MMC, filename);
}

// ============================================================================
// Filesystem + filename
// ============================================================================
OSIZE
bool AudioPlaySdWav::play(fs::FS &fs, const char *filename) {
    stop();
    closeSource();

    if (filename == nullptr) {
        ESP_LOGE(TAG, "Filename is null");
        return false;
    }

    ESP_LOGV(TAG, "Opening WAV file: %s", filename);

    AudioSourceFile *fileSource = new AudioSourceFile();

    if (fileSource == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate AudioSourceFile");
        return false;
    }

    if (!fileSource->open(fs, filename, FILE_READ)) {
        ESP_LOGE(TAG, "Failed to open WAV file: %s", filename);

        delete fileSource;
        return false;
    }

    ESP_LOGV(TAG, "File opened, size=%lu bytes",
             (unsigned long)fileSource->size());

    /*
     * Der Player übernimmt die Source.
     */
    return startPlayback(*fileSource, true);
}

// ============================================================================
// Generic AudioSource
// ============================================================================

bool AudioPlaySdWav::play(AudioSource &newSource) {
    stop();
    closeSource();
    return startPlayback(newSource, false);
}

// ============================================================================
// Common playback startup
// ============================================================================
OSIZE
bool AudioPlaySdWav::startPlayback(AudioSource &newSource, bool takeOwnership) {
    if (!newSource.isOpen()) {
        ESP_LOGE(TAG, "Audio source is not open");

        /*
         * Falls der Player die Source gerade selbst erzeugt hat,
         * muss sie hier wieder freigegeben werden.
         */
        if (takeOwnership) {
            newSource.close();
            delete &newSource;
        }

        return false;
    }

    source = &newSource;
    own_source = takeOwnership;

    ESP_LOGV(TAG, "Opening WAV source, size=%llu bytes",
             (unsigned long long)source->size());

    if (!parseWav()) {
        ESP_LOGE(TAG, "WAV parsing failed");

        closeSource();
        return false;
    }

    ESP_LOGV(TAG,
             "WAV accepted: %lu Hz, %u channel(s), %u bit, data=%llu bytes",
             (unsigned long)sample_rate, channels, bits_per_sample,
             (unsigned long long)data_length);

    if (sample_rate != 11025 && sample_rate != 22050 && sample_rate != 44100) {
        ESP_LOGE(TAG, "Unsupported sample rate: %lu Hz",
                 (unsigned long)sample_rate);

        closeSource();
        return false;
    }

    if (channels != 1 && channels != 2) {
        ESP_LOGE(TAG, "Unsupported channel count: %u", channels);

        closeSource();
        return false;
    }

    if (bits_per_sample != 8 && bits_per_sample != 16 &&
        bits_per_sample != 24 && bits_per_sample != 32) {
        ESP_LOGE(TAG, "Unsupported bits per sample: %u", bits_per_sample);

        closeSource();
        return false;
    }

    if (!seekAbsolute(data_start)) {
        ESP_LOGE(TAG, "Failed to seek to audio data at %llu",
                 (unsigned long long)data_start);

        closeSource();
        return false;
    }

    source_frame = 0;
    source_frames_read = 0;

    have_previous = false;
    have_next = false;

    source_phase = 0.0f;
    source_ended = false;
    float initialRate;
    portENTER_CRITICAL(&playback_rate_mux);
    initialRate = target_playback_rate;
    current_playback_rate = initialRate;
    portEXIT_CRITICAL(&playback_rate_mux);

    state = PLAYING;

    ESP_LOGV(TAG, "Playback started: source=%lu Hz, output=%.1f Hz, rate=%.3f",
             (unsigned long)sample_rate, (double)AUDIO_SAMPLE_RATE_EXACT,
             (double)initialRate);

    return true;
}

// ============================================================================
// Load an entire PCM WAV into PSRAM
// ============================================================================

bool AudioPlaySdWav::loadToMemory(fs::FS &fs, const char *filename,
                                  AudioSampleBuffer &buffer, size_t reserveBytes) {
    if (filename == nullptr || state != STOPPED || source != nullptr) {
        ESP_LOGE(TAG, "WAV preload requires a stopped player with no active source");
        return false;
    }

    AudioSourceFile *fileSource = new AudioSourceFile();
    if (fileSource == nullptr) {
        ESP_LOGE(TAG, "Could not allocate WAV source");
        return false;
    }

    if (!fileSource->open(fs, filename, FILE_READ)) {
        ESP_LOGE(TAG, "Cannot open WAV for PSRAM preload: %s", filename);
        delete fileSource;
        return false;
    }

    source = fileSource;
    own_source = true;

    if (!parseWav()) {
        ESP_LOGE(TAG, "WAV parsing failed during PSRAM preload");
        closeSource();
        return false;
    }

    if ((sample_rate != 11025 && sample_rate != 22050 && sample_rate != 44100) ||
        channels == 0 || block_align == 0 || data_length == 0 ||
        data_length % block_align != 0) {
        ESP_LOGE(TAG, "Unsupported WAV format or sample length for memory playback");
        closeSource();
        return false;
    }

    const uint64_t frames64 = data_length / block_align;
    if (frames64 == 0 || frames64 > UINT32_MAX ||
        frames64 > (uint64_t)SIZE_MAX / sizeof(int16_t)) {
        ESP_LOGE(TAG, "WAV sample is too large for AudioPlayMemory");
        closeSource();
        return false;
    }

    const uint32_t frames = (uint32_t)frames64;
    const size_t bytesNeeded = (size_t)frames * sizeof(int16_t);
    constexpr uint32_t PSRAM_CAPS = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    const size_t freeBytes = heap_caps_get_free_size(PSRAM_CAPS);
    const size_t largestBlock = heap_caps_get_largest_free_block(PSRAM_CAPS);
    if (freeBytes < bytesNeeded ||
        freeBytes - bytesNeeded < reserveBytes ||
        largestBlock < bytesNeeded) {
        ESP_LOGW(TAG,
                 "Not enough contiguous PSRAM for %s (%u needed, %u free, %u largest, %u reserve)",
                 filename, (unsigned)bytesNeeded, (unsigned)freeBytes,
                 (unsigned)largestBlock, (unsigned)reserveBytes);
        closeSource();
        return false;
    }

    int16_t *newSamples = static_cast<int16_t *>(
        heap_caps_malloc(bytesNeeded, PSRAM_CAPS));
    if (newSamples == nullptr) {
        ESP_LOGW(TAG, "PSRAM allocation failed (%u bytes)", (unsigned)bytesNeeded);
        closeSource();
        return false;
    }

    if (!seekAbsolute(data_start)) {
        ESP_LOGE(TAG, "Could not seek to WAV data for preload");
        heap_caps_free(newSamples);
        closeSource();
        return false;
    }

    source_frames_read = 0;
    for (uint32_t i = 0; i < frames; ++i) {
        int32_t left;
        int32_t right;
        if (!readSourceFrame(left, right)) {
            ESP_LOGE(TAG, "Unexpected end of WAV while preloading sample");
            heap_caps_free(newSamples);
            closeSource();
            return false;
        }

        // WAV decoding already scales all supported PCM depths to the upper
        // 16 bits of int32_t. Downmix stereo to mono for AudioPlayMemory.
        const int32_t left16 = left >> 16;
        const int32_t right16 = right >> 16;
        newSamples[i] = (int16_t)((left16 + right16) / 2);
    }

    closeSource();
    have_previous = false;
    have_next = false;
    source_ended = false;
    state = STOPPED;

    // Preserve the old buffer unless the entire new sample was loaded successfully.
    buffer.release();
    buffer.samples_ = newSamples;
    buffer.sample_count_ = frames;
    buffer.sample_rate_ = sample_rate;
    buffer.is_psram_ = true;

    ESP_LOGI(TAG, "Loaded %s into PSRAM: %u mono samples, %lu Hz, %u bytes",
             filename, (unsigned)frames, (unsigned long)sample_rate,
             (unsigned)bytesNeeded);
    return true;
}

// ============================================================================
// Playback rate
// ============================================================================

bool AudioPlaySdWav::setPlaybackRate(float rate) {
    if (!(rate >= 0.25f && rate <= 4.0f)) {
        return false;
    }

    portENTER_CRITICAL(&playback_rate_mux);
    target_playback_rate = rate;
    portEXIT_CRITICAL(&playback_rate_mux);
    return true;
}

float AudioPlaySdWav::playbackRate() const {
    portENTER_CRITICAL(&playback_rate_mux);
    const float result = target_playback_rate;
    portEXIT_CRITICAL(&playback_rate_mux);
    return result;
}

void AudioPlaySdWav::setLoop(bool enabled) {
    portENTER_CRITICAL(&playback_rate_mux);
    loop_enabled = enabled;
    portEXIT_CRITICAL(&playback_rate_mux);
}

bool AudioPlaySdWav::isLooping() const {
    portENTER_CRITICAL(&playback_rate_mux);
    const bool result = loop_enabled;
    portEXIT_CRITICAL(&playback_rate_mux);
    return result;
}

// ============================================================================
// Stop
// ============================================================================

void AudioPlaySdWav::stop() {
    if (state != STOPPED) {
        releaseBlocks();
        state = STOPPED;
    }

    /*
     * Wichtig:
     *
     * Eine externe Source darf hier NICHT geschlossen werden.
     *
     * Eine vom Player besessene Source wird ebenfalls nicht
     * hier freigegeben, weil closeSource() beim nächsten play()
     * bzw. im Destruktor aufgerufen wird.
     */
    have_previous = false;
    have_next = false;
}

// ============================================================================
// Play / Pause
// ============================================================================

void AudioPlaySdWav::togglePlayPause() {
    if (state == PLAYING) {
        state = PAUSED;
    } else if (state == PAUSED) {
        state = PLAYING;
    }
}

// ============================================================================
// State
// ============================================================================

bool AudioPlaySdWav::isPlaying() const {
    return state == PLAYING;
}

bool AudioPlaySdWav::isPaused() {
    return state == PAUSED;
}

bool AudioPlaySdWav::isStopped() {
    return state == STOPPED;
}

// ============================================================================
// Position
// ============================================================================

uint32_t AudioPlaySdWav::positionMillis() const {
    if (sample_rate == 0 || block_align == 0) {
        return 0;
    }

    const uint64_t bytesPlayed = (uint64_t)source_frames_read * block_align;
    return (uint32_t)((bytesPlayed * 1000ULL) /
                      ((uint64_t)sample_rate * block_align));
}

// ============================================================================
// Length
// ============================================================================

uint32_t AudioPlaySdWav::lengthMillis() const {
    if (sample_rate == 0 || block_align == 0) {
        return 0;
    }

    const uint32_t frames = data_length / block_align;
    return (uint32_t)(((uint64_t)frames * 1000ULL) / sample_rate);
}

// ============================================================================
// Source read
// ============================================================================

bool AudioPlaySdWav::readExact(void *buffer, size_t length) {
    uint8_t *p = (uint8_t *)(buffer);

    while (length > 0) {
        size_t received = 0;

        const AudioSourceStatus status = source->read(p, length, received);

        if (received > 0) {
            p += received;
            length -= received;

            continue;
        }

        switch (status) {
        case AudioSourceStatus::DATA:
            /*
             * DATA ohne Bytes sollte nicht vorkommen.
             */
            ESP_LOGE(TAG, "Source returned DATA without data");
            return false;

        case AudioSourceStatus::WOULD_BLOCK:
            /*
             * Für AudioSourceFile normalerweise nicht relevant.
             * Für spätere Netzwerk-Sources ist dies aber ein gültiger
             * Zustand.
             */
            delay(1);
            break;

        case AudioSourceStatus::END_OF_STREAM:

            ESP_LOGE(TAG, "Unexpected end of audio source");
            return false;

        case AudioSourceStatus::ERROR:

        default:
            ESP_LOGE(TAG, "Audio source read error");
            return false;
        }
    }

    return true;
}

// ============================================================================
// Seek
// ============================================================================

bool AudioPlaySdWav::seekAbsolute(uint64_t position) {
    if (source == nullptr) {
        return false;
    }

    if (!source->isSeekable()) {
        ESP_LOGE(TAG, "Audio source is not seekable");
        return false;
    }

    if (!source->seek(position)) {
        ESP_LOGE(TAG, "seek(%llu) failed, current position=%llu",
                 (unsigned long long)position,
                 (unsigned long long)source->position());
        return false;
    }

    return true;
}

// ============================================================================
// Skip
// ============================================================================
OSIZE
bool AudioPlaySdWav::skip(uint64_t length) {
    if (source == nullptr) {
        return false;
    }

    const uint64_t current = source->position();
    const uint64_t target = current + length + (length & 1ULL);
    return seekAbsolute(target);
}

// ============================================================================
// fmt chunk
// ============================================================================
OSIZE
bool AudioPlaySdWav::parseFmtChunk(uint64_t position, uint32_t size) {
    if (size < 16) {
        ESP_LOGE(TAG, "'fmt ' chunk too small: %lu bytes", (unsigned long)size);
        return false;
    }

    uint8_t fmt[40] = {};

    if (!seekAbsolute(position)) {
        ESP_LOGE(TAG, "Could not seek to fmt chunk");
        return false;
    }

    const size_t bytesToRead = size < sizeof(fmt) ? size : sizeof(fmt);
    if (!readExact(fmt, bytesToRead)) {
        ESP_LOGE(TAG, "Could not read fmt chunk");
        return false;
    }

    const uint16_t format = readLE16(fmt + 0);
    channels = readLE16(fmt + 2);
    sample_rate = readLE32(fmt + 4);
    const uint32_t byte_rate = readLE32(fmt + 8);
    block_align = readLE16(fmt + 12);
    bits_per_sample = readLE16(fmt + 14);

    ESP_LOGV(TAG, "WAV fmt chunk:");
    ESP_LOGV(TAG, "  format       = %u", format);
    ESP_LOGV(TAG, "  channels     = %u", channels);
    ESP_LOGV(TAG, "  sample rate  = %lu", (unsigned long)sample_rate);
    ESP_LOGV(TAG, "  byte rate    = %lu", (unsigned long)byte_rate);
    ESP_LOGV(TAG, "  block align  = %u", block_align);
    ESP_LOGV(TAG, "  bits/sample  = %u", bits_per_sample);

    if (format == WAV_PCM) {
        ESP_LOGV(TAG, "  format type  = PCM");
    } else if (format == WAV_EXTENSIBLE) {
        ESP_LOGV(TAG, "  format type  = WAVE_FORMAT_EXTENSIBLE");

        if (size < 40 || bytesToRead < 40) {
            ESP_LOGE(TAG, "WAVE_FORMAT_EXTENSIBLE fmt chunk is too small");

            return false;
        }

        const uint16_t cbSize = readLE16(fmt + 16);
        ESP_LOGV(TAG, "  extension    = %u bytes", cbSize);
        const uint32_t subFormat = readLE32(fmt + 24);
        ESP_LOGV(TAG, "  SubFormat    = 0x%08lX", (unsigned long)subFormat);

        if (subFormat != 1) {
            ESP_LOGE(TAG, "Unsupported WAVE_FORMAT_EXTENSIBLE SubFormat");
            return false;
        }
    } else {
        ESP_LOGE(TAG, "Unsupported WAV format code: %u", format);
        return false;
    }

    if (channels != 1 && channels != 2) {
        ESP_LOGE(TAG, "Unsupported channel count: %u", channels);
        return false;
    }

    if (bits_per_sample != 8 && bits_per_sample != 16 &&
        bits_per_sample != 24 && bits_per_sample != 32) {
        ESP_LOGE(TAG, "Unsupported bits/sample: %u", bits_per_sample);
        return false;
    }

    bytes_per_sample = (uint16_t)((bits_per_sample + 7) / 8);

    const uint16_t expectedBlockAlign = bytes_per_sample * channels;

    if (block_align != expectedBlockAlign) {
        ESP_LOGE(TAG, "Invalid block align: file=%u expected=%u", block_align,
                 expectedBlockAlign);
        return false;
    }

    if (byte_rate != sample_rate * block_align) {
        ESP_LOGE(TAG, "Invalid byte rate: file=%lu expected=%lu",
                 (unsigned long)byte_rate,
                 (unsigned long)(sample_rate * block_align));
        return false;
    }

    return true;
}

// ============================================================================
// Find data chunk
// ============================================================================

bool AudioPlaySdWav::findDataChunk() {
    const uint32_t fileSize = (uint32_t)source->size();

    while ((uint64_t)source->position() + 8 <= fileSize) {
        uint8_t chunk[8];

        const uint32_t chunkPosition = (uint32_t)source->position();

        if (!readExact(chunk, sizeof(chunk))) {
            ESP_LOGE(TAG, "Failed reading chunk header at %lu",
                     (unsigned long)chunkPosition);
            return false;
        }

        const uint32_t id = readLE32(chunk);
        const uint32_t size = readLE32(chunk + 4);

        printChunk(id, size, chunkPosition);

        if (id == DATA_ID) {
            data_start = source->position();
            data_length = size;
            total_length = size;

            ESP_LOGV(TAG, "Found data chunk: start=%lu size=%lu",
                     (unsigned long)data_start, (unsigned long)data_length);

            if (data_start + (uint64_t)data_length > fileSize) {
                ESP_LOGE(TAG, "data chunk exceeds file size");
                return false;
            }
            return true;
        }

        /*
         * Genau wie in der bisherigen funktionierenden
         * WAV-Implementierung.
         */
        const uint32_t next = size + (size & 1U);

        if (!skip(next)) {
            ESP_LOGE(TAG, "Could not skip chunk '%lu' bytes",
                     (unsigned long)next);

            return false;
        }
    }

    ESP_LOGE(TAG, "No 'data' chunk found");

    return false;
}

// ============================================================================
// WAV parser
// ============================================================================
OSIZE
bool AudioPlaySdWav::parseWav() {
    uint8_t header[12];

    if (!seekAbsolute(0)) {
        ESP_LOGE(TAG, "Could not seek to beginning of WAV file");

        return false;
    }

    if (!readExact(header, sizeof(header))) {
        ESP_LOGE(TAG, "Could not read RIFF header");

        return false;
    }

    const uint32_t riff = readLE32(header + 0);
    const uint32_t riffSize = readLE32(header + 4);
    const uint32_t wave = readLE32(header + 8);

    ESP_LOGV(TAG, "RIFF header: id=0x%08lX size=%lu type=0x%08lX",
             (unsigned long)riff, (unsigned long)riffSize, (unsigned long)wave);

    if (riff != RIFF_ID) {
        ESP_LOGE(TAG, "Not a RIFF file: id=0x%08lX", (unsigned long)riff);
        return false;
    }

    if (wave != WAVE_ID) {
        ESP_LOGE(TAG, "RIFF file is not WAVE: type=0x%08lX",
                 (unsigned long)wave);
        return false;
    }

    bool fmtFound = false;
    const uint32_t fileSize = (uint32_t)source->size();

    while ((uint64_t)source->position() + 8 <= fileSize) {
        uint8_t chunk[8];

        const uint32_t chunkPosition = (uint32_t)source->position();

        if (!readExact(chunk, sizeof(chunk))) {
            ESP_LOGE(TAG, "Failed reading chunk header at %lu",
                     (unsigned long)chunkPosition);
            return false;
        }

        const uint32_t id = readLE32(chunk);
        const uint32_t size = readLE32(chunk + 4);

        printChunk(id, size, chunkPosition);

        if (id == FMT_ID) {
            if (!parseFmtChunk(source->position(), size)) {
                ESP_LOGE(TAG, "Parsing fmt chunk failed");

                return false;
            }

            fmtFound = true;
            break;
        }

        if (!skip(size)) {
            ESP_LOGE(TAG, "Could not skip pre-fmt chunk");

            return false;
        }
    }

    if (!fmtFound) {
        ESP_LOGE(TAG, "No 'fmt ' chunk found");

        return false;
    }

    /*
     * Continue from the chunk after fmt.
     */
    if (!findDataChunk()) {
        ESP_LOGE(TAG, "Could not find audio data");

        return false;
    }

    return true;
}

// ============================================================================
// Decode sample
// ============================================================================
OSPEED
int32_t AudioPlaySdWav::decodeSample(const uint8_t *data) const {
    switch (bits_per_sample) {
    case 8:

        // WAV 8-bit PCM is unsigned.
        return (((int32_t)data[0] - 128) << 24);

    case 16:

        return (
            ((int32_t)(int16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8)))
            << 16);

    case 24: {
        int32_t value = (int32_t)data[0] | ((int32_t)data[1] << 8) |
                        ((int32_t)data[2] << 16);

        if (value & 0x00800000) {
            value |= (int32_t)0xFF000000;
        }

        return value << 8;
    }

    case 32:

        return (int32_t)((uint32_t)data[0] | ((uint32_t)data[1] << 8) |
                         ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24));

    default:

        return 0;
    }
}

// ============================================================================
// Read source frame
// ============================================================================

bool AudioPlaySdWav::readSourceFrame(int32_t &left, int32_t &right) {
    if ((uint64_t)source_frames_read * block_align >= data_length) {
        return false;
    }

    uint8_t frame[8];

    if (!readExact(frame, block_align)) {
        ESP_LOGE(TAG, "Failed reading audio frame %lu",
                 (unsigned long)source_frames_read);

        return false;
    }

    left = decodeSample(frame);

    if (channels == 2) {
        right = decodeSample(frame + bytes_per_sample);
    } else {
        right = left;
    }

    ++source_frames_read;

    return true;
}

bool AudioPlaySdWav::readSourceFrameOrLoop(int32_t &left, int32_t &right, bool loop) {
    // Restart only at the data-chunk boundary; never scan the RIFF header in the
    // audio update path. Seeking is done by the current file-backed WAV path.
    if ((uint64_t)source_frames_read * block_align >= data_length) {
        if (!loop || data_length == 0 || !seekAbsolute(data_start)) {
            return false;
        }
        source_frames_read = 0;
        source_frame = 0;
    }

    return readSourceFrame(left, right);
}

// ============================================================================
// Output sample / variable-rate interpolation
// ============================================================================
OSPEED
bool AudioPlaySdWav::getOutputSample(int16_t &left, int16_t &right,
                                     float targetRate, bool loop) {
    if (!have_previous) {
        if (!readSourceFrameOrLoop(previous_left, previous_right, loop)) {
            return false;
        }

        have_previous = true;
        have_next = false;
        source_ended = false;
        source_phase = 0.0f;
    }

    if (!have_next && !source_ended) {
        if (readSourceFrameOrLoop(next_left, next_right, loop)) {
            have_next = true;
        } else {
            source_ended = true;
        }
    }

    const float fraction = have_next ? source_phase : 0.0f;
    const int64_t leftDelta = have_next ? (int64_t)next_left - previous_left : 0;
    const int64_t rightDelta = have_next ? (int64_t)next_right - previous_right : 0;
    const int64_t l = (int64_t)previous_left +
                      (have_next ? (int64_t)(leftDelta * fraction) : 0);
    const int64_t r = (int64_t)previous_right +
                      (have_next ? (int64_t)(rightDelta * fraction) : 0);

    left = (int16_t)(l >> 16);
    right = (int16_t)(r >> 16);

    // The target may be updated by the control task; smooth the change here.
    current_playback_rate +=
        (targetRate - current_playback_rate) * 0.0015f;

    const float sourceStep =
        ((float)sample_rate / AUDIO_SAMPLE_RATE_EXACT) * current_playback_rate;
    source_phase += sourceStep;

    while (source_phase >= 1.0f) {
        source_phase -= 1.0f;

        if (have_next) {
            previous_left = next_left;
            previous_right = next_right;
            have_next = readSourceFrameOrLoop(next_left, next_right, loop);
            if (!have_next) {
                source_ended = true;
            }
        } else if (source_ended) {
            // The final source frame has been played for its full interval.
            have_previous = false;
            source_phase = 0.0f;
            break;
        }
    }

    return true;
}

// ============================================================================
// Release blocks
// ============================================================================

void AudioPlaySdWav::releaseBlocks() {
    if (block_left) {
        release(block_left);
        block_left = nullptr;
    }

    if (block_right) {
        release(block_right);
        block_right = nullptr;
    }

    block_offset = 0;
}

// ============================================================================
// Finish playback
// ============================================================================

void AudioPlaySdWav::finishPlayback() {
    if (block_left) {
        for (uint16_t i = block_offset; i < AUDIO_BLOCK_SAMPLES; ++i) {
            block_left->data[i] = 0;
        }

        transmit(block_left, 0);

        release(block_left);
        block_left = nullptr;
    }

    if (block_right) {
        for (uint16_t i = block_offset; i < AUDIO_BLOCK_SAMPLES; ++i) {
            block_right->data[i] = 0;
        }

        transmit(block_right, 1);

        release(block_right);
        block_right = nullptr;
    }

    /*
     * Hier NICHT closeSource().
     *
     * Bei einer externen Source gehört deren Lebensdauer
     * dem Aufrufer.
     */
    state = STOPPED;

    have_previous = false;
    have_next = false;

    block_offset = 0;
}

// ============================================================================
// Audio update
// ============================================================================
OSPEED
void AudioPlaySdWav::update() {
    if (state != PLAYING) {
        return;
    }

    block_left = allocate();

    if (!block_left) {
        ESP_LOGE(TAG, "Audio block allocation failed: left");

        return;
    }

    block_right = allocate();

    if (!block_right) {
        ESP_LOGE(TAG, "Audio block allocation failed: right");

        release(block_left);
        block_left = nullptr;

        return;
    }

    block_offset = 0;

    portENTER_CRITICAL(&playback_rate_mux);
    const float targetRate = target_playback_rate;
    const bool loop = loop_enabled;
    portEXIT_CRITICAL(&playback_rate_mux);

    while (block_offset < AUDIO_BLOCK_SAMPLES) {
        int16_t left;
        int16_t right;

        if (!getOutputSample(left, right, targetRate, loop)) {
            finishPlayback();
            return;
        }

        block_left->data[block_offset] = left;
        block_right->data[block_offset] = right;

        ++block_offset;
    }

    transmit(block_left, 0);
    transmit(block_right, 1);

    release(block_left);
    release(block_right);

    block_left = nullptr;
    block_right = nullptr;
}

#else

/*
 * This file is the ESP32 implementation of AudioPlaySdWav.
 * The Teensy has its own independent Audio Library and therefore
 * does not use this implementation.
 */

#endif
