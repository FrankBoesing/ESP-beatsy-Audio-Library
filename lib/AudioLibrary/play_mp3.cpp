#include "play_mp3.h"

#include <SD_MMC.h>
#include <algorithm>
#include <cstring>

namespace {
constexpr const char *TAG = "AudioPlayMp3";
}

AudioPlayMp3::AudioPlayMp3() : AudioDecoderStream(MP3_PCM_BUFFER_SAMPLES) {}

OSIZE
AudioPlayMp3::~AudioPlayMp3() {
    stop();
    closeSource();
}

// ============================================================================
// Source lifetime
// ============================================================================
OSIZE
void AudioPlayMp3::closeSource() {
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

// ============================================================================
// Public play API
// ============================================================================
OSIZE
bool AudioPlayMp3::play(const char *filename) {
    return play(SD_MMC, filename);
}

OSIZE
bool AudioPlayMp3::play(fs::FS &fs, const char *filename) {
    stop();
    closeSource();

    _lastError = ERR_NONE;

    if (filename == nullptr) {
        _lastError = ERR_FILE_NOT_FOUND;

        ESP_LOGE(TAG, "filename is null");

        return false;
    }

    AudioSourceFile *fileSource = new AudioSourceFile();

    if (fileSource == nullptr) {
        _lastError = ERR_OUT_OF_MEMORY;
        return false;
    }

    if (!fileSource->open(fs, filename, FILE_READ)) {
        delete fileSource;

        _lastError = ERR_FILE_NOT_FOUND;

        ESP_LOGE(TAG, "failed to open MP3 file: %s", filename);

        return false;
    }

    return startPlayback(*fileSource, true);
}

OSIZE
bool AudioPlayMp3::play(AudioSource &source) {
    stop();
    closeSource();

    _lastError = ERR_NONE;

    return startPlayback(source, false);
}

// ============================================================================
// Start / stop
// ============================================================================
OSIZE
bool AudioPlayMp3::startPlayback(AudioSource &source, bool takeOwnership) {
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

    _inputPos = 0;
    _inputLeft = 0;
    _inputEof = false;
    _inputPrepared = false;

    std::memset(_input, 0, sizeof(_input));
    std::memset(&_frameInfo, 0, sizeof(_frameInfo));

    _sampleRate = 0;
    _channels = 0;
    _bitrate = 0;

#if SOFTCODEC_METRICS
    _decodeFrames = 0;
    _decodeTimeUsTotal = 0;
    _decodeAudioTimeUsTotal = 0;
    _decodeProcessorUsageMaxX100 = 0;
#endif

    _paused = false;
    _playing = false;

    if (_source->isSeekable() && !_source->seek(0)) {
        _lastError = ERR_FILE_NOT_FOUND;

        closeSource();
        return false;
    }

    if (!_decoder.AllocateBuffers()) {
        _lastError = ERR_OUT_OF_MEMORY;

        closeSource();
        return false;
    }

    if (!startDecoderTask()) {
        _lastError = ERR_OUT_OF_MEMORY;

        _decoder.FreeBuffers();

        closeSource();
        return false;
    }

    _playing = true;

    return true;
}

OSIZE
void AudioPlayMp3::stop() {
    /*
     * Mark playback stopped immediately for the public API. The base-class
     * shutdown below still waits for the realtime update() and decoder task
     * before any PCM storage is freed.
     */
    _playing = false;

    stopDecoderTask();

    _decoder.FreeBuffers();

    _inputPos = 0;
    _inputLeft = 0;
    _inputEof = false;
    _inputPrepared = false;

    _paused = false;
}

bool AudioPlayMp3::isPlaying() const {
    /*
     * AudioStream::active describes graph participation, not playback state:
     * AudioConnection::connect() can set it before play() is called.
     *
     * _playing is therefore the actual playback state. It is cleared by
     * stop() and by AudioDecoderStream::onPlaybackFinished() after the final
     * READY PCM buffer has drained.
     */
        return _playing && !_paused &&
            !(decoderFinished() && decoderError() != 0);
}

// ============================================================================
// Position / length
// ============================================================================

uint32_t AudioPlayMp3::positionMillis() const {
    const uint32_t rate =
        _sampleRate != 0 ? _sampleRate
                         : static_cast<uint32_t>(AudioStream::sampleRate());

    if (rate == 0) {
        return 0;
    }

    return static_cast<uint32_t>((samplesPlayed() * 1000ULL) / rate);
}

uint32_t AudioPlayMp3::lengthMillis() const {
    if (_source == nullptr || _bitrate == 0) {
        return 0;
    }

    const uint64_t millis = (_source->size() * 8000ULL) / _bitrate;

    return static_cast<uint32_t>(millis);
}

// ============================================================================
// Decoder diagnostics
// ============================================================================

#if SOFTCODEC_METRICS

float AudioPlayMp3::decodeProcessorUsage() const {
    const uint64_t audioTimeUs = _decodeAudioTimeUsTotal;

    if (audioTimeUs == 0) {
        return 0.0f;
    }

    /*
     * Result in hundredths of a percent:
     *
     *   usage[%] * 100 =
     *       decodeTimeUs * 10000
     *       --------------------
     *          audioTimeUs
     *
     * Integer arithmetic only.
     */
    const uint64_t usageX100 =
        (_decodeTimeUsTotal * 10000ULL + audioTimeUs / 2ULL) / audioTimeUs;

    return static_cast<float>(usageX100) * 0.01f;
}

float AudioPlayMp3::decodeProcessorUsageMax() const {
    return static_cast<float>(_decodeProcessorUsageMaxX100) * 0.01f;
}

uint32_t AudioPlayMp3::decodeFrames() const {
    return _decodeFrames;
}

uint64_t AudioPlayMp3::decodeTimeUsTotal() const {
    return _decodeTimeUsTotal;
}

#endif

// ============================================================================
// ID3
// ============================================================================
OSIZE
size_t AudioPlayMp3::id3TagSize(const uint8_t header[10]) {
    if (header == nullptr || header[0] != 'I' || header[1] != 'D' ||
        header[2] != '3') {
        return 0;
    }

    const uint32_t tagDataSize = ((uint32_t)(header[6] & 0x7F) << 21) |
                                 ((uint32_t)(header[7] & 0x7F) << 14) |
                                 ((uint32_t)(header[8] & 0x7F) << 7) |
                                 ((uint32_t)(header[9] & 0x7F));

    size_t total = 10U + static_cast<size_t>(tagDataSize);

    if ((header[5] & 0x10U) != 0) {
        total += 10U;
    }

    return total;
}

// ============================================================================
// Compressed input buffer
// ============================================================================

bool AudioPlayMp3::fillInput(size_t minimumBytes) {
    if (_source == nullptr || minimumBytes > MP3_INPUT_BUFFER_SIZE) {
        return false;
    }

    while (_inputLeft < minimumBytes) {
        if (_inputPos > 0) {
            if (_inputLeft > 0) {
                std::memmove(_input, _input + _inputPos, _inputLeft);
            }

            _inputPos = 0;
        }

        if (_inputLeft >= minimumBytes) {
            return true;
        }

        const size_t freeSpace = MP3_INPUT_BUFFER_SIZE - _inputLeft;

        if (freeSpace == 0) {
            return false;
        }

        size_t received = 0;

        const AudioSourceStatus status =
            _source->read(_input + _inputLeft, freeSpace, received);

        if (received > 0) {
            _inputLeft += received;

            if (_inputLeft >= minimumBytes) {
                return true;
            }
        }

        switch (status) {
        case AudioSourceStatus::DATA:
            if (received == 0) {
                vTaskDelay(1);
            }
            break;

        case AudioSourceStatus::WOULD_BLOCK:
            vTaskDelay(1);
            break;

        case AudioSourceStatus::END_OF_STREAM:
            _inputEof = true;
            return _inputLeft >= minimumBytes;

        case AudioSourceStatus::ERROR:
        default:
            _lastError = ERR_SOURCE;
            return false;
        }
    }

    return true;
}

bool AudioPlayMp3::skipInput(size_t bytes) {
    while (bytes > 0) {
        if (_inputLeft == 0) {
            if (!fillInput(1)) {
                return false;
            }
        }

        const size_t consume = std::min(bytes, _inputLeft);

        _inputPos += consume;
        _inputLeft -= consume;

        bytes -= consume;
    }

    return true;
}

OSIZE
bool AudioPlayMp3::prepareMp3Input() {
    if (!fillInput(10)) {
        if (_lastError != ERR_SOURCE) {
            _lastError = ERR_FILE_NOT_FOUND;
        }
        return false;
    }

    const uint8_t *header = _input + _inputPos;

    const size_t tagSize = id3TagSize(header);

    if (tagSize == 0) {
        _inputPrepared = true;
        return true;
    }

    ESP_LOGI(TAG, "ID3 tag detected: %u bytes", static_cast<unsigned>(tagSize));

    if (!skipInput(tagSize)) {
        if (_lastError != ERR_SOURCE) {
            _lastError = ERR_FILE_NOT_FOUND;
        }
        return false;
    }

    _inputPrepared = true;
    return true;
}

// ============================================================================
// Frame information
// ============================================================================

bool AudioPlayMp3::validateFrameInfo(const MP3FrameInfo &info) {
    if (info.bitsPerSample != 16 || info.nChans < 1 || info.nChans > 2) {
        _lastError = ERR_FORMAT;

        ESP_LOGE(TAG, "unsupported MP3 format: channels=%d bits=%d",
                 info.nChans, info.bitsPerSample);

        return false;
    }

    const uint32_t outputRate =
        static_cast<uint32_t>(AudioStream::sampleRate());

    if (info.samprate <= 0 ||
        static_cast<uint32_t>(info.samprate) != outputRate) {
        _lastError = ERR_FORMAT;

        ESP_LOGE(TAG, "MP3 sample rate %d Hz does not match audio rate %lu Hz",
                 info.samprate, static_cast<unsigned long>(outputRate));

        return false;
    }

    if (info.outputSamps <= 0 ||
        static_cast<size_t>(info.outputSamps) > MP3_PCM_BUFFER_SAMPLES) {
        _lastError = ERR_FORMAT;

        ESP_LOGE(TAG, "invalid MP3 output sample count: %d", info.outputSamps);

        return false;
    }

    _sampleRate = static_cast<uint32_t>(info.samprate);
    _channels = static_cast<uint16_t>(info.nChans);
    _bitrate = static_cast<uint32_t>(info.bitrate > 0 ? info.bitrate : 0);
    _frameInfo = info;

    return true;
}

// ============================================================================
// MP3 frame decoding
// ============================================================================

OSPEED
AudioPlayMp3::DecodeResult AudioPlayMp3::decodePcmBuffer(int16_t *destination,
                                                         size_t capacity,
                                                         size_t &outSamples) {
    outSamples = 0;

    if (!_decoder.IsInit() || destination == nullptr ||
        capacity < MP3_PCM_BUFFER_SAMPLES) {
        _lastError = ERR_DECODER;
        return DecodeResult::ERROR;
    }

    if (!_inputPrepared) {
        if (!prepareMp3Input()) {
            return DecodeResult::ERROR;
        }
    }

    for (;;) {
        if (!fillInput(4)) {
            if (_lastError == ERR_SOURCE) {
                return DecodeResult::ERROR;
            }

            return _inputEof ? DecodeResult::END_OF_STREAM
                             : DecodeResult::RETRY;
        }

        int offset =
            _decoder.MP3FindSyncWord(_input + _inputPos, static_cast<int>(_inputLeft));

        if (offset < 0) {
            if (_inputLeft > 3) {
                _inputPos += _inputLeft - 3;
                _inputLeft = 3;
            }

            if (_inputEof) {
                return DecodeResult::END_OF_STREAM;
            }

            if (!fillInput(4)) {
                if (_lastError == ERR_SOURCE) {
                    return DecodeResult::ERROR;
                }

                return _inputEof ? DecodeResult::END_OF_STREAM
                                 : DecodeResult::RETRY;
            }

            continue;
        }

        if (offset > 0) {
            _inputPos += static_cast<size_t>(offset);
            _inputLeft -= static_cast<size_t>(offset);
        }

        int32_t bytesLeft = static_cast<int32_t>(_inputLeft);
        const int32_t bytesBefore = bytesLeft;
        const uint32_t decodeStartUs = micros();
        const int decodeResult =
            _decoder.MP3Decode(_input + _inputPos, &bytesLeft, destination, 0);

#if SOFTCODEC_METRICS
        const uint32_t decodeElapsedUs = micros() - decodeStartUs;
#endif
        const int consumed = bytesBefore - bytesLeft;

        if (consumed < 0 || static_cast<size_t>(consumed) > _inputLeft) {
            _lastError = ERR_DECODER;

            ESP_LOGE(TAG, "MP3 decoder returned invalid byte count");

            return DecodeResult::ERROR;
        }

        if (decodeResult == ERR_MP3_NONE) {
            _inputPos += static_cast<size_t>(consumed);
            _inputLeft -= static_cast<size_t>(consumed);

            MP3FrameInfo info = {};

            info.outputSamps = _decoder.MP3GetOutputSamps();

            // Decoder skips output while the bit reservoir fills.
            if (info.outputSamps <= 0) {
                continue;
            }

            info.bitrate = _decoder.MP3GetBitrate();
            info.nChans = _decoder.MP3GetChannels();
            info.samprate = _decoder.MP3GetSampRate();
            info.bitsPerSample = _decoder.MP3GetBitsPerSample();
            info.layer = _decoder.MP3GetLayer();
            info.version = _decoder.MP3GetVersion();

            if (!validateFrameInfo(info)) {
                return DecodeResult::ERROR;
            }

            /*
             * Account successful MP3 frames only. The decoder time is
             * normalized against the actual audio duration represented by
             * each frame, so MPEG-1 (1152 samples/frame) and MPEG-2/2.5
             * (576 samples/frame) are handled correctly.
             */
#if SOFTCODEC_METRICS
            const uint64_t frameAudioSamples =
                (static_cast<uint64_t>(info.outputSamps) /
                 static_cast<uint64_t>(info.nChans));

            const uint64_t frameAudioTimeUs =
                info.samprate > 0 ? (frameAudioSamples * 1000000ULL) /
                                        static_cast<uint64_t>(info.samprate)
                                  : 0ULL;

            _decodeFrames = _decodeFrames + 1;
            _decodeTimeUsTotal += decodeElapsedUs;
            _decodeAudioTimeUsTotal += frameAudioTimeUs;

            if (frameAudioTimeUs > 0) {
                const uint64_t usageX100 =
                    (static_cast<uint64_t>(decodeElapsedUs) * 10000ULL +
                     frameAudioTimeUs / 2ULL) /
                    frameAudioTimeUs;

                if (usageX100 > _decodeProcessorUsageMaxX100) {
                    _decodeProcessorUsageMaxX100 =
                        static_cast<uint32_t>(usageX100);
                }
            }
#endif
            const size_t decoderSamples = static_cast<size_t>(info.outputSamps);

            if (info.nChans == 2) {
                outSamples = decoderSamples;
            } else {
                if (decoderSamples * 2U > capacity) {
                    _lastError = ERR_DECODER;

                    ESP_LOGE(TAG, "mono frame does not fit output buffer");

                    return DecodeResult::ERROR;
                }
                std::memmove(destination + decoderSamples, destination,
                             decoderSamples * sizeof(int16_t));

                outSamples = decoderSamples * 2U;
            }

            return outSamples > 0 ? DecodeResult::FILLED : DecodeResult::ERROR;
        }

        if (decodeResult == ERR_MP3_INDATA_UNDERFLOW) {
            if (_inputPos > 0) {
                std::memmove(_input, _input + _inputPos, _inputLeft);

                _inputPos = 0;
            }

            if (_inputEof) {
                return DecodeResult::END_OF_STREAM;
            }

            const size_t refillTarget =
                _source->fillToThreshold() ? MP3_STREAM_REFILL_TARGET
                                           : MP3_INPUT_BUFFER_SIZE;

            if (!fillInput(refillTarget)) {
                if (_lastError == ERR_SOURCE) {
                    return DecodeResult::ERROR;
                }

                return _inputEof ? DecodeResult::END_OF_STREAM
                                 : DecodeResult::RETRY;
            }

            continue;
        }

        if (decodeResult == ERR_MP3_MAINDATA_UNDERFLOW) {
            _inputPos += static_cast<size_t>(consumed);
            _inputLeft -= static_cast<size_t>(consumed);

            continue;
        }

        _inputPos += static_cast<size_t>(consumed);
        _inputLeft -= static_cast<size_t>(consumed);

        _lastError = decodeResult;

        ESP_LOGE(TAG, "MP3Decode failed: %d", decodeResult);

        return DecodeResult::ERROR;
    }
}
