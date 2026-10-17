#include "AudioSourceMemory.h"
#include "optimize.h"

OSIZE
AudioSourceMemory::AudioSourceMemory()
    : _data(nullptr), _size(0), _position(0) {}

OSIZE
AudioSourceMemory::AudioSourceMemory(const uint8_t *data, size_t size)
    : _data(nullptr), _size(0), _position(0) {
    open(data, size);
}

OSIZE
bool AudioSourceMemory::open(const uint8_t *data, size_t size) {
    close();

    if (!data || size == 0) {
        return false;
    }

    _data = data;
    _size = size;
    _position = 0;

    return true;
}

OSPEED
AudioSourceStatus AudioSourceMemory::read(uint8_t *buffer, size_t requested,
                                          size_t &received) {
    received = 0;

    if (!_data || !_size) {
        return AudioSourceStatus::ERROR;
    }

    if (!buffer || requested == 0) {
        return AudioSourceStatus::ERROR;
    }

    if (_position >= _size) {
        return AudioSourceStatus::END_OF_STREAM;
    }

    size_t remaining = _size - _position;
    size_t toRead = requested;

    if (toRead > remaining) {
        toRead = remaining;
    }

    memcpy(buffer, _data + _position, toRead);

    _position += toRead;
    received = toRead;

    return AudioSourceStatus::DATA;
}

OSIZE
uint64_t AudioSourceMemory::position() const {
    return _position;
}

OSIZE
uint64_t AudioSourceMemory::size() const {
    return _size;
}

OSIZE
bool AudioSourceMemory::isSeekable() const {
    return true;
}

OSIZE
bool AudioSourceMemory::seek(uint64_t position) {
    if (!_data) {
        return false;
    }

    if (position > _size) {
        return false;
    }

    _position = (size_t)(position);

    return true;
}

OSIZE
void AudioSourceMemory::close() {
    /*
     * Die Daten selbst gehören dem Aufrufer.
     * Daher werden sie hier NICHT freigegeben.
     */
    _data = nullptr;
    _size = 0;
    _position = 0;
}

OSIZE
bool AudioSourceMemory::isOpen() const {
    return _data != nullptr;
}
