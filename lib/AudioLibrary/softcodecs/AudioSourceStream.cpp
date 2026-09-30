#include "AudioSourceStream.h"

AudioSourceStream::AudioSourceStream() : _stream(nullptr), _position(0) {}

AudioSourceStream::AudioSourceStream(Stream &stream)
    : _stream(nullptr), _position(0) {
    open(stream);
}

bool AudioSourceStream::open(Stream &stream) {
    _stream = &stream;
    _position = 0;

    return true;
}

AudioSourceStatus AudioSourceStream::read(uint8_t *buffer, size_t requested,
                                          size_t &received) {
    received = 0;

    if (!_stream) {
        return AudioSourceStatus::ERROR;
    }

    if (!buffer || requested == 0) {
        return AudioSourceStatus::ERROR;
    }

    int available = _stream->available();

    if (available <= 0) {
        return AudioSourceStatus::WOULD_BLOCK;
    }

    size_t toRead = requested;

    if (static_cast<size_t>(available) < toRead) {
        toRead = static_cast<size_t>(available);
    }

    size_t n = _stream->readBytes(reinterpret_cast<char *>(buffer), toRead);

    received = n;
    _position += n;

    if (n > 0) {
        return AudioSourceStatus::DATA;
    }

    return AudioSourceStatus::WOULD_BLOCK;
}

uint64_t AudioSourceStream::position() const {
    return _position;
}

uint64_t AudioSourceStream::size() const {
    return 0;
}

bool AudioSourceStream::isSeekable() const {
    return false;
}

bool AudioSourceStream::seek(uint64_t position) {
    (void)position;
    return false;
}

void AudioSourceStream::close() {
    _stream = nullptr;
    _position = 0;
}

bool AudioSourceStream::isOpen() const {
    return _stream != nullptr;
}

size_t AudioSourceStream::refillThreshold() const {
    return 32 * 1024;
}

size_t AudioSourceStream::fillSize() const {
    return 32 * 1024;
}

bool AudioSourceStream::fillToThreshold() const {
    return true;
}

Stream *AudioSourceStream::stream() {
    return _stream;
}
