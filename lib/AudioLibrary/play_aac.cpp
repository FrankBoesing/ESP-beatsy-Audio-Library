#include "play_aac.h"

#include <SD_MMC.h>
#include <algorithm>
#include <cstring>

namespace {
constexpr const char *TAG = "AudioPlayAac";
}

AudioPlayAac::AudioPlayAac() : AudioDecoderStream(AAC_PCM_BUFFER_SAMPLES) {}

AudioPlayAac::~AudioPlayAac() {
    stop();
    closeSource();
}

OSIZE
void AudioPlayAac::closeSource() {
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

bool AudioPlayAac::play(const char *filename) {
    return play(SD_MMC, filename);
}

OSIZE
bool AudioPlayAac::play(fs::FS &fs, const char *filename) {
    stop();
    closeSource();

    _lastError = ERR_NONE;

    AudioSourceFile *fileSource = new AudioSourceFile();

    if (fileSource == nullptr) {
        _lastError = ERR_OUT_OF_MEMORY;
        return false;
    }

    if (!fileSource->open(fs, filename, FILE_READ)) {
        delete fileSource;
        _lastError = ERR_FILE_NOT_FOUND;
        ESP_LOGE(TAG, "failed to open AAC file: %s", filename);
        return false;
    }

    return startPlayback(*fileSource, true);
}

OSIZE
bool AudioPlayAac::play(AudioSource &source) {
    stop();
    closeSource();

    _lastError = ERR_NONE;
    return startPlayback(source, false);
}

OSIZE
bool AudioPlayAac::startPlayback(AudioSource &source, bool takeOwnership) {
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
    memset(_input, 0, sizeof(_input));
    memset(&_frameInfo, 0, sizeof(_frameInfo));

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
void AudioPlayAac::stop() {
    _playing = false;

    stopDecoderTask();

    if (_decoder.IsInit()) {
        _decoder.FreeBuffers();
    }

    _inputPos = 0;
    _inputLeft = 0;
    _inputEof = false;
    _inputPrepared = false;
    _paused = false;
}

bool AudioPlayAac::isPlaying() const {
    return _playing && !_paused;
}

uint32_t AudioPlayAac::positionMillis() const {
    const uint32_t rate = _sampleRate != 0 ? _sampleRate : (uint32_t)AudioStream::sampleRate();

    if (rate == 0) {
        return 0;
    }

    return (uint32_t)((samplesPlayed() * 1000ULL) / rate);
}

uint32_t AudioPlayAac::lengthMillis() const {
    if (_source == nullptr || _bitrate == 0) {
        return 0;
    }

    const uint64_t millis = (_source->size() * 8000ULL) / _bitrate;
    return (uint32_t)(millis);
}

#if SOFTCODEC_METRICS

float AudioPlayAac::decodeProcessorUsage() const {
    const uint64_t audioTimeUs = _decodeAudioTimeUsTotal;

    if (audioTimeUs == 0) {
        return 0.0f;
    }

    const uint64_t usageX100 = (_decodeTimeUsTotal * 10000ULL + audioTimeUs / 2ULL) / audioTimeUs;

    return (float)(usageX100) * 0.01f;
}

float AudioPlayAac::decodeProcessorUsageMax() const {
    return (float)(_decodeProcessorUsageMaxX100) * 0.01f;
}

uint32_t AudioPlayAac::decodeFrames() const {
    return _decodeFrames;
}

uint64_t AudioPlayAac::decodeTimeUsTotal() const {
    return _decodeTimeUsTotal;
}

#endif

OSIZE
size_t AudioPlayAac::id3TagSize(const uint8_t header[10]) {
    if (header == nullptr || header[0] != 'I' || header[1] != 'D' || header[2] != '3') {
        return 0;
    }

    const size_t tagDataSize = ((size_t)(header[6] & 0x7F) << 21) | ((size_t)(header[7] & 0x7F) << 14) |
                               ((size_t)(header[8] & 0x7F) << 7) | ((size_t)(header[9] & 0x7F));

    size_t total = 10U + tagDataSize;
    if ((header[5] & 0x10U) != 0) {
        total += 10U;
    }

    return total;
}

bool AudioPlayAac::fillInput(size_t minimumBytes) {
    if (_source == nullptr || minimumBytes > AAC_INPUT_BUFFER_SIZE) {
        _lastError = ERR_FILE_NOT_FOUND;
        return false;
    }

    while (_inputLeft < minimumBytes) {
        if (_inputPos > 0) {
            if (_inputLeft > 0) {
                memmove(_input, _input + _inputPos, _inputLeft);
            }

            _inputPos = 0;
        }

        if (_inputLeft >= minimumBytes) {
            return true;
        }

        const size_t freeSpace = AAC_INPUT_BUFFER_SIZE - _inputLeft;

        if (freeSpace == 0) {
            return false;
        }

        size_t received = 0;
        const AudioSourceStatus status = _source->read(_input + _inputLeft, freeSpace, received);

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
            _lastError = ERR_FILE_NOT_FOUND;
            return false;
        }
    }

    return true;
}

bool AudioPlayAac::skipInput(size_t bytes) {
    while (bytes > 0) {
        if (_inputLeft == 0 && !fillInput(1)) {
            return false;
        }

        const size_t consume = std::min(bytes, _inputLeft);
        _inputPos += consume;
        _inputLeft -= consume;
        bytes -= consume;
    }

    return true;
}

OSIZE
bool AudioPlayAac::prepareAacInput() {
    if (!fillInput(10)) {
        if (_lastError == ERR_NONE) {
            _lastError = ERR_FILE_NOT_FOUND;
        }
        return false;
    }

    const uint8_t *header = _input + _inputPos;
    const size_t tagSize = id3TagSize(header);

    if (tagSize > 0) {
        ESP_LOGV(TAG, "ID3 tag detected: %lu bytes", tagSize);

        if (!skipInput(tagSize)) {
            if (_lastError == ERR_NONE) {
                _lastError = ERR_FILE_NOT_FOUND;
            }
            return false;
        }
    }

    _inputPrepared = true;
    return true;
}

bool AudioPlayAac::validateFrameInfo(const AACFrameInfo &info) {
    if (info.bitsPerSample != 16 || info.nChans < 1 || info.nChans > 2) {
        _lastError = ERR_FORMAT;
        ESP_LOGE(TAG, "unsupported AAC format: channels=%d bits=%d", info.nChans, info.bitsPerSample);
        return false;
    }

    const uint32_t outputRate = (uint32_t)(AudioStream::sampleRate());

    if (info.sampRateOut <= 0 || (uint32_t)(info.sampRateOut) != outputRate) {
        _lastError = ERR_FORMAT;
        ESP_LOGE(TAG, "AAC sample rate %d Hz does not match audio rate %lu Hz", info.sampRateOut,
                 (unsigned long)(outputRate));
        return false;
    }

    if (info.outputSamps <= 0 || (size_t)(info.outputSamps) > AAC_PCM_BUFFER_SAMPLES ||
        info.outputSamps % info.nChans != 0) {
        _lastError = ERR_FORMAT;
        ESP_LOGE(TAG, "invalid AAC output sample count: %d", info.outputSamps);
        return false;
    }

    _sampleRate = info.sampRateOut;
    _channels = info.nChans;
    _bitrate = info.bitRate > 0 ? info.bitRate : 0;
    _frameInfo = info;
    return true;
}

OSPEED
AudioDecoderStream::DecodeResult AudioPlayAac::decodePcmBuffer(int16_t *destination, size_t capacity,
                                                               size_t &outSamples) {
    outSamples = 0;

    if (!_decoder.IsInit() || destination == nullptr || capacity < AAC_PCM_BUFFER_SAMPLES) {
        _lastError = ERR_DECODER;
        return DecodeResult::ERROR;
    }

    if (!_inputPrepared && !prepareAacInput()) {
        return DecodeResult::ERROR;
    }

    for (;;) {
        if (!_inputEof && _inputLeft < AAC_MIN_FRAME_BYTES) {
            if (!fillInput(AAC_MIN_FRAME_BYTES) && !_inputEof) {
                return _lastError == ERR_NONE ? DecodeResult::RETRY : DecodeResult::ERROR;
            }
        }

        if (_inputLeft < 4) {
            return _inputEof ? DecodeResult::END_OF_STREAM : DecodeResult::RETRY;
        }

        unsigned char *input = _input + _inputPos;
        int32_t bytesLeft = (int32_t)_inputLeft;
        const int bytesBefore = bytesLeft;
        const uint32_t decodeStartUs = micros();
        const int decodeResult = _decoder.Decode(&input, &bytesLeft, (short *)destination);

#if SOFTCODEC_METRICS
        const uint32_t decodeElapsedUs = micros() - decodeStartUs;
#endif
        const int consumed = bytesBefore - bytesLeft;

        if (consumed < 0 || (size_t)consumed > _inputLeft) {
            _lastError = ERR_DECODER;
            ESP_LOGE(TAG, "AAC decoder returned invalid byte count");
            return DecodeResult::ERROR;
        }

        if (decodeResult == ERR_AAC_NONE) {
            if (consumed == 0) {
                _lastError = ERR_DECODER;
                ESP_LOGE(TAG, "AAC decoder consumed no input bytes");
                return DecodeResult::ERROR;
            }

            _inputPos += (size_t)consumed;
            _inputLeft -= (size_t)consumed;

            AACFrameInfo info = {};
            _decoder.GetLastFrameInfo(&info);

            if (!validateFrameInfo(info)) {
                return DecodeResult::ERROR;
            }

#if SOFTCODEC_METRICS
            const uint64_t frameAudioSamples =
                (uint64_t)(info.outputSamps) / (uint64_t)(info.nChans);
            const uint64_t frameAudioTimeUs =
                (frameAudioSamples * 1000000ULL) / (uint64_t)(info.sampRateOut);

            _decodeFrames = _decodeFrames + 1;
            _decodeTimeUsTotal += decodeElapsedUs;
            _decodeAudioTimeUsTotal += frameAudioTimeUs;

            if (frameAudioTimeUs > 0) {
                const uint64_t usageX100 =
                    ((uint64_t)(decodeElapsedUs) * 10000ULL + frameAudioTimeUs / 2ULL) / frameAudioTimeUs;

                if (usageX100 > _decodeProcessorUsageMaxX100) {
                    _decodeProcessorUsageMaxX100 = (uint32_t)(usageX100);
                }
            }
#endif

            const size_t decoderSamples = (size_t)info.outputSamps;

            if (info.nChans == 2) {
                outSamples = decoderSamples;
            } else {
                if (decoderSamples * 2U > capacity) {
                    _lastError = ERR_DECODER;
                    ESP_LOGE(TAG, "mono frame does not fit output buffer");
                    return DecodeResult::ERROR;
                }
                //copy left to right channel
                memcpy(destination + decoderSamples, destination, decoderSamples * sizeof(int16_t));
                outSamples = decoderSamples * 2U;
            }

            return outSamples > 0 ? DecodeResult::FILLED : DecodeResult::ERROR;
        }

        if (decodeResult == ERR_AAC_INDATA_UNDERFLOW) {
            if (_inputEof) {
                return DecodeResult::END_OF_STREAM;
            }

            _lastError = ERR_DECODER;
            ESP_LOGE(TAG, "AAC decoder underflow with %lu buffered bytes; refusing unsafe retry", _inputLeft);
            return DecodeResult::ERROR;
        }

        _lastError = decodeResult;
        ESP_LOGE(TAG, "AACDecode failed: %d", decodeResult);
        return DecodeResult::ERROR;
    }
}
