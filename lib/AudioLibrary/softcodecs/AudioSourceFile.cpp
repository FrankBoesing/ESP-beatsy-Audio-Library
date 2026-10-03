#include "AudioSourceFile.h"

AudioSourceFile::AudioSourceFile() {}

AudioSourceFile::AudioSourceFile(const File &file) {
    open(file);
}

AudioSourceFile::~AudioSourceFile() {
    close();
}

bool AudioSourceFile::open(const File &file) {
    close();

    if (!file) {
        return false;
    }

    _file = file;

    return static_cast<bool>(_file);
}

bool AudioSourceFile::open(fs::FS &fs, const char *path, const char *mode) {
    close();

    if (!path) {
        return false;
    }

    _file = fs.open(path, mode);

    return static_cast<bool>(_file);
}

AudioSourceStatus AudioSourceFile::read(uint8_t *buffer, size_t requested,
                                        size_t &received) {
    received = 0;

    if (!_file) {
        return AudioSourceStatus::ERROR;
    }

    if (!buffer || requested == 0) {
        return AudioSourceStatus::ERROR;
    }

    size_t n = _file.read(buffer, requested);
    received = n;

    if (n > 0) {
        return AudioSourceStatus::DATA;
    }

    if (_file.available() == 0) {
        return AudioSourceStatus::END_OF_STREAM;
    }

    return AudioSourceStatus::WOULD_BLOCK;
}

uint64_t AudioSourceFile::position() const {
    if (!_file) {
        return 0;
    }

    return static_cast<uint64_t>(_file.position());
}

uint64_t AudioSourceFile::size() const {
    if (!_file) {
        return 0;
    }

    return static_cast<uint64_t>(_file.size());
}

bool AudioSourceFile::isSeekable() const {
    return static_cast<bool>(_file);
}

bool AudioSourceFile::seek(uint64_t position) {
    if (!_file) {
        return false;
    }

    /*
     * Annahme:
     * Die Audio-Dateien bleiben zunächst unter 4 GiB.
     * Arduino-ESP32 File::seek() verwendet hier eine 32-Bit-Position.
     */
    if (position > UINT32_MAX) {
        return false;
    }

    return _file.seek(static_cast<uint32_t>(position));
}

void AudioSourceFile::close() {
    if (_file) {
        _file.close();
    }
}

bool AudioSourceFile::isOpen() const {
    return static_cast<bool>(_file);
}

size_t AudioSourceFile::refillThreshold() const {
    return 2 * 1024;
}

size_t AudioSourceFile::fillSize() const {
    return 2 * 1024;
}

bool AudioSourceFile::fillToThreshold() const {
    return false;
}

File &AudioSourceFile::file() {
    return _file;
}

const File &AudioSourceFile::file() const {
    return _file;
}
