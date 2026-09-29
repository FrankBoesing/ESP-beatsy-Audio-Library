#include <Arduino.h>
#include "play_sd_wav.h"

#if defined(ARDUINO_ARCH_ESP32)

static const char *TAG = "AudioPlaySdWav";

namespace {

constexpr uint32_t RIFF_ID = 0x46464952UL; // "RIFF"
constexpr uint32_t WAVE_ID = 0x45564157UL; // "WAVE"
constexpr uint32_t FMT_ID  = 0x20746D66UL; // "fmt "
constexpr uint32_t DATA_ID = 0x61746164UL; // "data"

constexpr uint16_t WAV_PCM        = 1;
constexpr uint16_t WAV_EXTENSIBLE = 0xFFFE;

uint16_t readLE16(const uint8_t *p)
{
    return (uint16_t)p[0] |
           ((uint16_t)p[1] << 8);
}

uint32_t readLE32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

void printChunk(uint32_t id, uint32_t size, uint32_t position)
{
    char name[5];

    name[0] = (char)(id & 0xFF);
    name[1] = (char)((id >> 8) & 0xFF);
    name[2] = (char)((id >> 16) & 0xFF);
    name[3] = (char)((id >> 24) & 0xFF);
    name[4] = 0;

    ESP_LOGI(TAG,
             "chunk '%s': position=%lu size=%lu",
             name,
             (unsigned long)position,
             (unsigned long)size);
}

} // namespace


void AudioPlaySdWav::begin()
{
    state = STOPPED;

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

    resample_phase = 0;
    resample_step = 1;

    releaseBlocks();
}


bool AudioPlaySdWav::play(const char *filename)
{
    stop();

    ESP_LOGI(TAG, "Opening WAV file: %s", filename);

    wavfile = SD_MMC.open(filename, FILE_READ);

    if (!wavfile) {
        ESP_LOGE(TAG, "Failed to open WAV file: %s", filename);
        return false;
    }

    ESP_LOGI(TAG,
             "File opened, size=%lu bytes",
             (unsigned long)wavfile.size());

    if (!parseWav()) {
        ESP_LOGE(TAG, "WAV parsing failed");
        wavfile.close();
        return false;
    }

    ESP_LOGI(TAG,
             "WAV accepted: %lu Hz, %u channel(s), %u bit, data=%lu bytes",
             (unsigned long)sample_rate,
             channels,
             bits_per_sample,
             (unsigned long)data_length);

    if (sample_rate != 11025 &&
        sample_rate != 22050 &&
        sample_rate != 44100) {

        ESP_LOGE(TAG,
                 "Unsupported sample rate: %lu Hz",
                 (unsigned long)sample_rate);

        wavfile.close();
        return false;
    }

    if (channels != 1 && channels != 2) {
        ESP_LOGE(TAG,
                 "Unsupported channel count: %u",
                 channels);

        wavfile.close();
        return false;
    }

    if (bits_per_sample != 8 &&
        bits_per_sample != 16 &&
        bits_per_sample != 24 &&
        bits_per_sample != 32) {

        ESP_LOGE(TAG,
                 "Unsupported bits per sample: %u",
                 bits_per_sample);

        wavfile.close();
        return false;
    }

    if (!seekAbsolute(data_start)) {
        ESP_LOGE(TAG,
                 "Failed to seek to audio data at %lu",
                 (unsigned long)data_start);

        wavfile.close();
        return false;
    }

    source_frame = 0;
    source_frames_read = 0;

    have_previous = false;
    have_next = false;

    resample_phase = 0;

    /*
     * Output rate is AUDIO_SAMPLE_RATE_EXACT.
     *
     * For the first implementation the supported source rates
     * are exact integer divisors of 44.1 kHz.
     */
    if (sample_rate == AUDIO_SAMPLE_RATE_EXACT) {
        resample_step = 1;
    }
    else if (sample_rate == 22050 &&
             AUDIO_SAMPLE_RATE_EXACT == 44100) {
        resample_step = 2;
    }
    else if (sample_rate == 11025 &&
             AUDIO_SAMPLE_RATE_EXACT == 44100) {
        resample_step = 4;
    }
    else {
        ESP_LOGE(TAG,
                 "Source rate %lu cannot be resampled to audio rate %.1f",
                 (unsigned long)sample_rate,
                 (double)AUDIO_SAMPLE_RATE_EXACT);

        wavfile.close();
        return false;
    }

    state = PLAYING;

    ESP_LOGI(TAG,
             "Playback started, output rate=%.1f Hz, resample step=%lu",
             (double)AUDIO_SAMPLE_RATE_EXACT,
             (unsigned long)resample_step);

    return true;
}


void AudioPlaySdWav::stop()
{
    if (state != STOPPED) {
        releaseBlocks();
        state = STOPPED;
    }

    if (wavfile) {
        wavfile.close();
    }

    have_previous = false;
    have_next = false;
}


void AudioPlaySdWav::togglePlayPause()
{
    if (state == PLAYING) {
        state = PAUSED;
    }
    else if (state == PAUSED) {
        state = PLAYING;
    }
}


bool AudioPlaySdWav::isPlaying()
{
    return state == PLAYING;
}


bool AudioPlaySdWav::isPaused()
{
    return state == PAUSED;
}


bool AudioPlaySdWav::isStopped()
{
    return state == STOPPED;
}


uint32_t AudioPlaySdWav::positionMillis()
{
    if (sample_rate == 0 || block_align == 0) {
        return 0;
    }

    const uint32_t bytesPlayed =
        source_frames_read * block_align;

    return (uint32_t)(
        ((uint64_t)bytesPlayed * 1000ULL) /
        ((uint64_t)sample_rate * block_align)
    );
}


uint32_t AudioPlaySdWav::lengthMillis()
{
    if (sample_rate == 0 || block_align == 0) {
        return 0;
    }

    const uint32_t frames =
        data_length / block_align;

    return (uint32_t)(
        ((uint64_t)frames * 1000ULL) /
        sample_rate
    );
}


bool AudioPlaySdWav::readExact(void *buffer, size_t length)
{
    uint8_t *p = static_cast<uint8_t *>(buffer);

    while (length > 0) {

        const int n = wavfile.read(p, length);

        if (n <= 0) {
            ESP_LOGE(TAG,
                     "File read failed: requested=%u",
                     (unsigned)length);
            return false;
        }

        p += n;
        length -= (size_t)n;
    }

    return true;
}


bool AudioPlaySdWav::seekAbsolute(uint32_t position)
{
    if (!wavfile.seek(position)) {
        ESP_LOGE(TAG,
                 "seek(%lu) failed, current position=%lu",
                 (unsigned long)position,
                 (unsigned long)wavfile.position());
        return false;
    }

    return true;
}


bool AudioPlaySdWav::skip(uint32_t length)
{
    const uint32_t current =
        (uint32_t)wavfile.position();

    const uint32_t target =
        current + length + (length & 1U);

    return seekAbsolute(target);
}


bool AudioPlaySdWav::parseFmtChunk(uint32_t position,
                                   uint32_t size)
{
    if (size < 16) {
        ESP_LOGE(TAG,
                 "'fmt ' chunk too small: %lu bytes",
                 (unsigned long)size);
        return false;
    }

    uint8_t fmt[40] = {};

    if (!seekAbsolute(position)) {
        ESP_LOGE(TAG, "Could not seek to fmt chunk");
        return false;
    }

    const size_t bytesToRead =
        size < sizeof(fmt) ? size : sizeof(fmt);

    if (!readExact(fmt, bytesToRead)) {
        ESP_LOGE(TAG, "Could not read fmt chunk");
        return false;
    }

    const uint16_t format =
        readLE16(fmt + 0);

    channels =
        readLE16(fmt + 2);

    sample_rate =
        readLE32(fmt + 4);

    const uint32_t byte_rate =
        readLE32(fmt + 8);

    block_align =
        readLE16(fmt + 12);

    bits_per_sample =
        readLE16(fmt + 14);

    ESP_LOGI(TAG, "WAV fmt chunk:");
    ESP_LOGI(TAG, "  format       = %u", format);
    ESP_LOGI(TAG, "  channels     = %u", channels);
    ESP_LOGI(TAG, "  sample rate  = %lu", (unsigned long)sample_rate);
    ESP_LOGI(TAG, "  byte rate    = %lu", (unsigned long)byte_rate);
    ESP_LOGI(TAG, "  block align  = %u", block_align);
    ESP_LOGI(TAG, "  bits/sample  = %u", bits_per_sample);

    if (format == WAV_PCM) {
        ESP_LOGI(TAG, "  format type  = PCM");
    }
    else if (format == WAV_EXTENSIBLE) {

        ESP_LOGI(TAG, "  format type  = WAVE_FORMAT_EXTENSIBLE");

        if (size < 40 || bytesToRead < 40) {
            ESP_LOGE(TAG,
                     "WAVE_FORMAT_EXTENSIBLE fmt chunk is too small");
            return false;
        }

        const uint16_t cbSize =
            readLE16(fmt + 16);

        ESP_LOGI(TAG,
                 "  extension    = %u bytes",
                 cbSize);

        /*
         * SubFormat GUID begins at offset 24.
         * For PCM the first DWORD is 1.
         */
        const uint32_t subFormat =
            readLE32(fmt + 24);

        ESP_LOGI(TAG,
                 "  SubFormat    = 0x%08lX",
                 (unsigned long)subFormat);

        if (subFormat != 1) {
            ESP_LOGE(TAG,
                     "Unsupported WAVE_FORMAT_EXTENSIBLE SubFormat");
            return false;
        }
    }
    else {
        ESP_LOGE(TAG,
                 "Unsupported WAV format code: %u",
                 format);
        return false;
    }

    if (channels != 1 && channels != 2) {
        ESP_LOGE(TAG,
                 "Unsupported channel count: %u",
                 channels);
        return false;
    }

    if (bits_per_sample != 8 &&
        bits_per_sample != 16 &&
        bits_per_sample != 24 &&
        bits_per_sample != 32) {

        ESP_LOGE(TAG,
                 "Unsupported bits/sample: %u",
                 bits_per_sample);
        return false;
    }

    bytes_per_sample =
        (uint16_t)((bits_per_sample + 7) / 8);

    const uint16_t expectedBlockAlign =
        bytes_per_sample * channels;

    if (block_align != expectedBlockAlign) {
        ESP_LOGE(TAG,
                 "Invalid block align: file=%u expected=%u",
                 block_align,
                 expectedBlockAlign);
        return false;
    }

    if (byte_rate != sample_rate * block_align) {
        ESP_LOGE(TAG,
                 "Invalid byte rate: file=%lu expected=%lu",
                 (unsigned long)byte_rate,
                 (unsigned long)(sample_rate * block_align));
        return false;
    }

    return true;
}


bool AudioPlaySdWav::findDataChunk()
{
    const uint32_t fileSize =
        (uint32_t)wavfile.size();

    while ((uint32_t)wavfile.position() + 8 <= fileSize) {

        uint8_t chunk[8];

        const uint32_t chunkPosition =
            (uint32_t)wavfile.position();

        if (!readExact(chunk, sizeof(chunk))) {
            ESP_LOGE(TAG,
                     "Failed reading chunk header at %lu",
                     (unsigned long)chunkPosition);
            return false;
        }

        const uint32_t id =
            readLE32(chunk);

        const uint32_t size =
            readLE32(chunk + 4);

        printChunk(id, size, chunkPosition);

        if (id == DATA_ID) {

            data_start =
                (uint32_t)wavfile.position();

            data_length = size;
            total_length = size;

            ESP_LOGI(TAG,
                     "Found data chunk: start=%lu size=%lu",
                     (unsigned long)data_start,
                     (unsigned long)data_length);

            if ((uint64_t)data_start + data_length > fileSize) {
                ESP_LOGE(TAG,
                         "data chunk exceeds file size");
                return false;
            }

            return true;
        }

        /*
         * Unknown chunks are legal in a RIFF/WAVE file.
         * The chunk payload is padded to an even number of bytes.
         */
        const uint32_t next =
            size + (size & 1U);

        if (!skip(next)) {
            ESP_LOGE(TAG,
                     "Could not skip chunk '%lu' bytes",
                     (unsigned long)next);
            return false;
        }
    }

    ESP_LOGE(TAG, "No 'data' chunk found");
    return false;
}


bool AudioPlaySdWav::parseWav()
{
    uint8_t header[12];

    if (!seekAbsolute(0)) {
        ESP_LOGE(TAG, "Could not seek to beginning of WAV file");
        return false;
    }

    if (!readExact(header, sizeof(header))) {
        ESP_LOGE(TAG, "Could not read RIFF header");
        return false;
    }

    const uint32_t riff =
        readLE32(header + 0);

    const uint32_t riffSize =
        readLE32(header + 4);

    const uint32_t wave =
        readLE32(header + 8);

    ESP_LOGI(TAG,
             "RIFF header: id=0x%08lX size=%lu type=0x%08lX",
             (unsigned long)riff,
             (unsigned long)riffSize,
             (unsigned long)wave);

    if (riff != RIFF_ID) {
        ESP_LOGE(TAG,
                 "Not a RIFF file: id=0x%08lX",
                 (unsigned long)riff);
        return false;
    }

    if (wave != WAVE_ID) {
        ESP_LOGE(TAG,
                 "RIFF file is not WAVE: type=0x%08lX",
                 (unsigned long)wave);
        return false;
    }

    bool fmtFound = false;

    const uint32_t fileSize =
        (uint32_t)wavfile.size();

    while ((uint32_t)wavfile.position() + 8 <= fileSize) {

        uint8_t chunk[8];

        const uint32_t chunkPosition =
            (uint32_t)wavfile.position();

        if (!readExact(chunk, sizeof(chunk))) {
            ESP_LOGE(TAG,
                     "Failed reading chunk header at %lu",
                     (unsigned long)chunkPosition);
            return false;
        }

        const uint32_t id =
            readLE32(chunk);

        const uint32_t size =
            readLE32(chunk + 4);

        printChunk(id, size, chunkPosition);

        if (id == FMT_ID) {

            if (!parseFmtChunk(
                    (uint32_t)wavfile.position(),
                    size)) {

                ESP_LOGE(TAG,
                         "Parsing fmt chunk failed");
                return false;
            }

            fmtFound = true;

            break;
        }

        if (!skip(size)) {
            ESP_LOGE(TAG,
                     "Could not skip pre-fmt chunk");
            return false;
        }
    }

    if (!fmtFound) {
        ESP_LOGE(TAG,
                 "No 'fmt ' chunk found");
        return false;
    }

    /*
     * Continue from the chunk after fmt.
     * findDataChunk() logs every intervening chunk.
     */
    if (!findDataChunk()) {
        ESP_LOGE(TAG,
                 "Could not find audio data");
        return false;
    }

    return true;
}


int32_t AudioPlaySdWav::decodeSample(const uint8_t *data) const
{
    switch (bits_per_sample) {

        case 8:
            // WAV 8-bit PCM is unsigned.
            return ((int32_t)data[0] - 128) << 24;

        case 16:
            return ((int32_t)(int16_t)(
                (uint16_t)data[0] |
                ((uint16_t)data[1] << 8)
            )) << 16;

        case 24: {
            int32_t value =
                (int32_t)data[0] |
                ((int32_t)data[1] << 8) |
                ((int32_t)data[2] << 16);

            if (value & 0x00800000) {
                value |= (int32_t)0xFF000000;
            }

            return value << 8;
        }

        case 32:
            return (int32_t)(
                (uint32_t)data[0] |
                ((uint32_t)data[1] << 8) |
                ((uint32_t)data[2] << 16) |
                ((uint32_t)data[3] << 24)
            );

        default:
            return 0;
    }
}


bool AudioPlaySdWav::readSourceFrame(int32_t &left,
                                     int32_t &right)
{
    if (source_frames_read * block_align >= data_length) {
        return false;
    }

    uint8_t frame[8];

    if (!readExact(frame, block_align)) {
        ESP_LOGE(TAG,
                 "Failed reading audio frame %lu",
                 (unsigned long)source_frames_read);
        return false;
    }

    left = decodeSample(frame);

    if (channels == 2) {
        right = decodeSample(
            frame + bytes_per_sample);
    }
    else {
        right = left;
    }

    ++source_frames_read;

    return true;
}


bool AudioPlaySdWav::getOutputSample(int16_t &left,
                                     int16_t &right)
{
    /*
     * First get two source samples. The interpolation between
     * them is only relevant for 22.05 / 11.025 kHz.
     */
    if (!have_previous) {

        if (!readSourceFrame(
                previous_left,
                previous_right)) {
            return false;
        }

        have_previous = true;

        if (resample_step > 1) {

            if (readSourceFrame(
                    next_left,
                    next_right)) {
                have_next = true;
            }
        }
    }

    if (resample_step == 1) {

        left  = (int16_t)(previous_left >> 16);
        right = (int16_t)(previous_right >> 16);

        have_previous = false;

        return true;
    }

    if (!have_next) {
        return false;
    }

    const uint32_t phase =
        resample_phase % resample_step;

    const int64_t l =
        (int64_t)previous_left +
        ((int64_t)(next_left - previous_left) *
         phase) / resample_step;

    const int64_t r =
        (int64_t)previous_right +
        ((int64_t)(next_right - previous_right) *
         phase) / resample_step;

    left  = (int16_t)(l >> 16);
    right = (int16_t)(r >> 16);

    ++resample_phase;

    if ((resample_phase % resample_step) == 0) {

        previous_left  = next_left;
        previous_right = next_right;

        if (readSourceFrame(
                next_left,
                next_right)) {
            have_next = true;
        }
        else {
            have_next = false;
        }
    }

    return true;
}


void AudioPlaySdWav::releaseBlocks()
{
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


void AudioPlaySdWav::finishPlayback()
{
    if (block_left) {

        for (uint16_t i = block_offset;
             i < AUDIO_BLOCK_SAMPLES;
             ++i) {
            block_left->data[i] = 0;
        }

        transmit(block_left, 0);
        release(block_left);
        block_left = nullptr;
    }

    if (block_right) {

        for (uint16_t i = block_offset;
             i < AUDIO_BLOCK_SAMPLES;
             ++i) {
            block_right->data[i] = 0;
        }

        transmit(block_right, 1);
        release(block_right);
        block_right = nullptr;
    }

    wavfile.close();

    state = STOPPED;
    have_previous = false;
    have_next = false;
}


void AudioPlaySdWav::update()
{
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

    while (block_offset < AUDIO_BLOCK_SAMPLES) {

        int16_t left;
        int16_t right;

        if (!getOutputSample(left, right)) {
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
