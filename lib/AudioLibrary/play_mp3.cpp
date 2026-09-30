#include "play_mp3.h"
#include "defines.h"

#include <SD_MMC.h>
#include <algorithm>
#include <cstring>

namespace {
constexpr const char *TAG = "AudioPlayMp3";

/*
 * Maximum Helix MP3 frame output:
 *   MAX_NCHAN * MAX_NGRAN * MAX_NSAMP
 *
 * Stereo/interleaved PCM therefore occupies exactly this many int16_t
 * values for the largest MPEG1 stereo frame.
 */
constexpr size_t MP3_PCM_BUFFER_SAMPLES =
    MAX_NCHAN * MAX_NGRAN * MAX_NSAMP;

constexpr size_t MP3_INPUT_BUFFER_SIZE =
    32U * 1024U;
}

AudioPlayMp3::AudioPlayMp3()
    : AudioDecoderStream(MP3_PCM_BUFFER_SAMPLES),
      _inputBuffer(MP3_INPUT_BUFFER_SIZE)
{
    /*
     * Intentionally empty:
     * no decoder and no task are created here.
     */
}

AudioPlayMp3::~AudioPlayMp3()
{
    stop();
    closeSource();
}

void AudioPlayMp3::closeSource()
{
    if (_source == nullptr) {
        _ownSource = false;
        return;
    }

    if (_ownSource) {
        _source->close();
        delete _source;
    }

    _source = nullptr;
    _ownSource = false;
}

bool AudioPlayMp3::play(const char *filename)
{
    return play(SD_MMC, filename);
}

bool AudioPlayMp3::play(
    fs::FS &fs,
    const char *filename
)
{
    stop();
    closeSource();

    _lastError = ERR_NONE;

    if (filename == nullptr) {
        _lastError = ERR_FILE_NOT_FOUND;
        ESP_LOGE(TAG, "filename is null");
        return false;
    }

    AudioSourceFile *fileSource =
        new AudioSourceFile();

    if (fileSource == nullptr) {
        _lastError = ERR_OUT_OF_MEMORY;
        return false;
    }

    if (!fileSource->open(
            fs,
            filename,
            FILE_READ))
    {
        delete fileSource;
        _lastError = ERR_FILE_NOT_FOUND;

        ESP_LOGE(
            TAG,
            "failed to open MP3 file: %s",
            filename
        );

        return false;
    }

    return startPlayback(
        *fileSource,
        true
    );
}

bool AudioPlayMp3::play(AudioSource &source)
{
    stop();
    closeSource();

    _lastError = ERR_NONE;

    return startPlayback(
        source,
        false
    );
}

bool AudioPlayMp3::startPlayback(
    AudioSource &source,
    bool takeOwnership
)
{
    if (!source.isOpen()) {

        if (takeOwnership) {
            source.close();
            delete &source;
        }

        _lastError = ERR_FILE_NOT_FOUND;
        return false;
    }

    _source = &source;
    _ownSource = takeOwnership;

    _inputBuffer.end();

    if (!_inputBuffer.begin()) {
        _lastError = ERR_OUT_OF_MEMORY;
        closeSource();
        return false;
    }

    /*
     * Decoder initialization is playback startup work. Actual MP3Decode()
     * calls only happen after startDecoderTask() has created the task.
     */
    _decoder = MP3InitDecoder();

    if (_decoder == nullptr) {
        _lastError = ERR_OUT_OF_MEMORY;
        _inputBuffer.end();
        closeSource();
        return false;
    }

    std::memset(
        &_frameInfo,
        0,
        sizeof(_frameInfo)
    );

    _sampleRate = 0;
    _channels = 0;
    _bitrate = 0;
    _dataSamplesPlayed = 0;
    _inputPrepared = false;
    _paused = false;

    if (_source->isSeekable() &&
        !_source->seek(0))
    {
        _lastError = ERR_FILE_NOT_FOUND;

        MP3FreeDecoder(_decoder);
        _decoder = nullptr;

        _inputBuffer.end();
        closeSource();
        return false;
    }

    if (!startDecoderTask()) {
        _lastError = ERR_OUT_OF_MEMORY;

        MP3FreeDecoder(_decoder);
        _decoder = nullptr;

        _inputBuffer.end();
        closeSource();
        return false;
    }

    return true;
}

void AudioPlayMp3::stop()
{
    /*
     * First stop the realtime side and wait for the decoder task.
     * Only then are the source, input buffer and decoder released.
     */
    stopDecoderTask();

    if (_decoder != nullptr) {
        MP3FreeDecoder(_decoder);
        _decoder = nullptr;
    }

    _inputBuffer.end();

    _inputPrepared = false;
    _paused = false;
    _dataSamplesPlayed = 0;

    _lastError = ERR_NONE;
}

bool AudioPlayMp3::isPlaying() const
{
    return isActive() &&
           !_paused &&
           !decoderFinished();
}

uint32_t AudioPlayMp3::positionMillis() const
{
    const uint32_t rate =
        _sampleRate != 0
            ? _sampleRate
            : static_cast<uint32_t>(
                AudioStream::sampleRate());

    if (rate == 0) {
        return 0;
    }

    return static_cast<uint32_t>(
        (samplesPlayed() * 1000ULL) / rate
    );
}

uint32_t AudioPlayMp3::lengthMillis() const
{
    if (_source == nullptr ||
        _bitrate == 0)
    {
        return 0;
    }

    /*
     * This is intentionally only an approximate length for VBR files,
     * matching the traditional codec-player behavior.
     */
    const uint64_t millis =
        (_source->size() * 8000ULL) /
        _bitrate;

    return static_cast<uint32_t>(millis);
}

size_t AudioPlayMp3::id3TagSize(
    const uint8_t header[10]
)
{
    if (header == nullptr ||
        header[0] != 'I' ||
        header[1] != 'D' ||
        header[2] != '3')
    {
        return 0;
    }

    /*
     * ID3v2 uses a 28-bit synchsafe size.
     */
    const uint32_t tagDataSize =
        ((uint32_t)(header[6] & 0x7F) << 21) |
        ((uint32_t)(header[7] & 0x7F) << 14) |
        ((uint32_t)(header[8] & 0x7F) << 7)  |
        ((uint32_t)(header[9] & 0x7F));

    size_t total =
        10U + static_cast<size_t>(tagDataSize);

    /*
     * ID3v2.4 footer-present flag.
     */
    if ((header[5] & 0x10U) != 0) {
        total += 10U;
    }

    return total;
}

bool AudioPlayMp3::appendInput(size_t requested)
{
    if (_source == nullptr ||
        requested == 0)
    {
        return false;
    }

    size_t writable = 0;

    uint8_t *destination =
        _inputBuffer.acquireWrite(writable);

    if (destination == nullptr ||
        writable == 0)
    {
        return false;
    }

    requested = std::min(requested, writable);

    size_t received = 0;

    const AudioSourceStatus status =
        _source->read(
            destination,
            requested,
            received
        );

    if (!_inputBuffer.commitWrite(received)) {
        return false;
    }

    if (status == AudioSourceStatus::DATA &&
        received > 0)
    {
        return true;
    }

    return false;
}

bool AudioPlayMp3::fillInput(size_t minimumBytes)
{
    if (_source == nullptr) {
        return false;
    }

    while (_inputBuffer.availableRead() < minimumBytes) {

        const size_t before =
            _inputBuffer.availableRead();

        /*
         * Prefer the existing generic refill policy.
         */
        const AudioSourceStatus status =
            _inputBuffer.fill(*_source);

        if (_inputBuffer.availableRead() >= minimumBytes) {
            return true;
        }

        const size_t after =
            _inputBuffer.availableRead();

        if (after > before) {
            continue;
        }

        /*
         * AudioInputBuffer deliberately refuses a normal refill once its
         * filesystem threshold is reached. For a decoder we occasionally
         * need more input than that (e.g. a frame spanning a refill).
         *
         * Use the buffer's write side directly. This still keeps all source
         * I/O in the decoder task and does not introduce another PCM buffer.
         */
        if (after > 0 &&
            after < _inputBuffer.capacity() &&
            status != AudioSourceStatus::END_OF_STREAM &&
            status != AudioSourceStatus::ERROR)
        {
            const size_t missing =
                minimumBytes - after;

            const size_t request =
                std::max<size_t>(missing, 512U);

            if (appendInput(request)) {
                continue;
            }
        }

        if (status == AudioSourceStatus::END_OF_STREAM ||
            status == AudioSourceStatus::ERROR)
        {
            return false;
        }

        /*
         * WOULD_BLOCK is allowed for future streaming AudioSource
         * implementations. Waiting is legal here because this is the
         * decoder task, never update().
         */
        vTaskDelay(1);
    }

    return true;
}

bool AudioPlayMp3::skipInput(size_t bytes)
{
    while (bytes > 0) {

        if (!fillInput(1)) {
            return false;
        }

        size_t available = 0;

        const uint8_t *data =
            _inputBuffer.acquireRead(available);

        if (data == nullptr ||
            available == 0)
        {
            return false;
        }

        const size_t consume =
            std::min(bytes, available);

        if (!_inputBuffer.releaseRead(consume)) {
            return false;
        }

        bytes -= consume;
    }

    return true;
}

bool AudioPlayMp3::prepareMp3Input()
{
    if (!fillInput(10)) {
        _lastError = ERR_FILE_NOT_FOUND;
        return false;
    }

    size_t length = 0;

    const uint8_t *header =
        _inputBuffer.acquireRead(length);

    if (header == nullptr ||
        length < 10)
    {
        _lastError = ERR_FILE_NOT_FOUND;
        return false;
    }

    const size_t tagSize =
        id3TagSize(header);

    if (tagSize == 0) {
        _inputPrepared = true;
        return true;
    }

    ESP_LOGI(
        TAG,
        "ID3 tag detected: %u bytes",
        static_cast<unsigned>(tagSize)
    );

    if (!skipInput(tagSize)) {
        _lastError = ERR_FILE_NOT_FOUND;
        return false;
    }

    _inputPrepared = true;
    return true;
}

bool AudioPlayMp3::validateFrameInfo(
    const MP3FrameInfo &info
)
{
    if (info.bitsPerSample != 16 ||
        info.nChans < 1 ||
        info.nChans > 2)
    {
        _lastError = ERR_FORMAT;

        ESP_LOGE(
            TAG,
            "unsupported MP3 format: channels=%d bits=%d",
            info.nChans,
            info.bitsPerSample
        );

        return false;
    }

    const uint32_t outputRate =
        static_cast<uint32_t>(
            AudioStream::sampleRate()
        );

    if (info.samprate <= 0 ||
        static_cast<uint32_t>(info.samprate) != outputRate)
    {
        _lastError = ERR_FORMAT;

        ESP_LOGE(
            TAG,
            "MP3 sample rate %d Hz does not match audio rate %lu Hz",
            info.samprate,
            static_cast<unsigned long>(outputRate)
        );

        return false;
    }

    _sampleRate =
        static_cast<uint32_t>(info.samprate);

    _channels =
        static_cast<uint16_t>(info.nChans);

    _bitrate =
        static_cast<uint32_t>(
            info.bitrate > 0
                ? info.bitrate
                : 0
        );

    _frameInfo = info;

    return true;
}

AudioPlayMp3::DecodeResult
AudioPlayMp3::decodePcmBuffer(
    int16_t *destination,
    size_t capacity,
    size_t &outSamples
)
{
    outSamples = 0;

    if (_decoder == nullptr ||
        destination == nullptr ||
        capacity < 2)
    {
        _lastError = ERR_DECODER;
        return DecodeResult::ERROR;
    }

    /*
     * Everything in this function runs exclusively in the decoder task.
     */
    if (!_inputPrepared) {
        if (!prepareMp3Input()) {
            return DecodeResult::END_OF_STREAM;
        }
    }

    while (outSamples < capacity) {

        /*
         * A valid MP3 frame needs at least enough bytes for its header.
         * More data is requested below whenever the decoder reports that a
         * complete frame is not currently available.
         */
        if (!fillInput(4)) {
            return DecodeResult::END_OF_STREAM;
        }

        size_t available = 0;

        const uint8_t *readPtr =
            _inputBuffer.acquireRead(available);

        if (readPtr == nullptr ||
            available < 4)
        {
            return DecodeResult::RETRY;
        }

        const int offset =
            MP3FindSyncWord(
                const_cast<unsigned char *>(
                    reinterpret_cast<const unsigned char *>(
                        readPtr
                    )
                ),
                static_cast<int>(available)
            );

        if (offset < 0) {
            /*
             * Preserve one byte for a sync word split across a refill.
             */
            if (available > 1) {
                if (!_inputBuffer.releaseRead(
                        available - 1))
                {
                    _lastError = ERR_DECODER;
                    return DecodeResult::ERROR;
                }
            }

            if (!appendInput(512U) &&
                _inputBuffer.availableRead() <= 1)
            {
                return DecodeResult::END_OF_STREAM;
            }

            continue;
        }

        if (offset > 0) {
            if (!_inputBuffer.releaseRead(
                    static_cast<size_t>(offset)))
            {
                _lastError = ERR_DECODER;
                return DecodeResult::ERROR;
            }

            continue;
        }

        /*
         * We are exactly at a frame sync word.
         */
        readPtr =
            _inputBuffer.acquireRead(available);

        if (readPtr == nullptr ||
            available < 4)
        {
            return DecodeResult::RETRY;
        }

        MP3FrameInfo nextInfo = {};

        const int infoResult =
            MP3GetNextFrameInfo(
                _decoder,
                &nextInfo,
                const_cast<unsigned char *>(
                    reinterpret_cast<const unsigned char *>(
                        readPtr
                    )
                )
            );

        if (infoResult == ERR_MP3_NONE &&
            nextInfo.outputSamps > 0)
        {
            const size_t frameOutputSamples =
                static_cast<size_t>(
                    nextInfo.outputSamps
                ) *
                (
                    nextInfo.nChans == 1
                        ? 2U
                        : 1U
                );

            if (frameOutputSamples >
                capacity - outSamples)
            {
                /*
                 * Never start a frame that cannot fit completely into this
                 * PCM buffer.
                 */
                return outSamples > 0
                    ? DecodeResult::FILLED
                    : DecodeResult::ERROR;
            }
        }

        unsigned char *input =
            const_cast<unsigned char *>(
                reinterpret_cast<const unsigned char *>(
                    readPtr
                )
            );

        int bytesLeft =
            static_cast<int>(available);

        const int beforeBytes =
            bytesLeft;

        const int result =
            MP3Decode(
                _decoder,
                &input,
                &bytesLeft,
                destination + outSamples,
                0
            );

        const int consumed =
            beforeBytes - bytesLeft;

        if (consumed > 0) {
            if (!_inputBuffer.releaseRead(
                    static_cast<size_t>(consumed)))
            {
                _lastError = ERR_DECODER;
                return DecodeResult::ERROR;
            }
        }

        if (result == ERR_MP3_NONE) {

            MP3FrameInfo info = {};

            MP3GetLastFrameInfo(
                _decoder,
                &info
            );

            if (!validateFrameInfo(info)) {
                return DecodeResult::ERROR;
            }

            const size_t frameSamples =
                static_cast<size_t>(
                    info.outputSamps
                );

            if (info.nChans == 2) {

                /*
                 * Stereo decoder output already is interleaved.
                 */
                outSamples += frameSamples;

            }
            else {

                /*
                 * Helix returns mono samples for a mono frame. Expand to
                 * stereo in-place, backwards so no source sample is lost.
                 */
                if (outSamples +
                        frameSamples * 2U >
                    capacity)
                {
                    _lastError = ERR_DECODER;
                    return DecodeResult::ERROR;
                }

                for (size_t i = frameSamples;
                     i > 0;
                     --i)
                {
                    const int16_t sample =
                        destination[
                            outSamples + i - 1U
                        ];

                    destination[
                        outSamples + (2U * i) - 2U
                    ] = sample;

                    destination[
                        outSamples + (2U * i) - 1U
                    ] = sample;
                }

                outSamples +=
                    frameSamples * 2U;
            }

            if (outSamples >= capacity) {
                return DecodeResult::FILLED;
            }

            continue;
        }

        if (result == ERR_MP3_INDATA_UNDERFLOW) {

            /*
             * The current compressed frame is incomplete. Refill more input
             * and retry from the same frame start.
             */
            if (!fillInput(
                    _inputBuffer.availableRead() + 1024U))
            {
                return outSamples > 0
                    ? DecodeResult::END_OF_STREAM
                    : DecodeResult::END_OF_STREAM;
            }

            continue;
        }

        if (result == ERR_MP3_MAINDATA_UNDERFLOW) {

            /*
             * This can occur for a stream started without enough preceding
             * bit-reservoir data. The original Teensy codec continues with
             * the next decoder step.
             */
            continue;
        }

        _lastError = result;

        ESP_LOGE(
            TAG,
            "MP3Decode failed: %d",
            result
        );

        return DecodeResult::ERROR;
    }

    return DecodeResult::FILLED;
}
