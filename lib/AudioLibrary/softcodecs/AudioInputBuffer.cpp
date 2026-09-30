#include "AudioInputBuffer.h"

#if defined(ARDUINO_ARCH_ESP32)
#include "esp_heap_caps.h"
#endif

AudioInputBuffer::AudioInputBuffer(size_t capacity)
    : buffer(nullptr), buffer_size(capacity), region_a_start(0),
      region_a_length(0), region_b_start(0), region_b_length(0),
      write_acquired(false), write_start(0), write_length(0),
      psram_allocated(false) {}

AudioInputBuffer::~AudioInputBuffer() {
    end();
}

bool AudioInputBuffer::begin() {
    end();

    if (buffer_size == 0) {
        return false;
    }

    if (!allocateBuffer()) {
        return false;
    }

    reset();

    return true;
}

void AudioInputBuffer::end() {
    freeBuffer();
    reset();
}

void AudioInputBuffer::reset() {
    region_a_start = 0;
    region_a_length = 0;

    region_b_start = 0;
    region_b_length = 0;

    write_acquired = false;
    write_start = 0;
    write_length = 0;
}

bool AudioInputBuffer::allocateBuffer() {
#if defined(ARDUINO_ARCH_ESP32)
    if (psramFound()) {
        buffer = static_cast<uint8_t *>(
            heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

        if (buffer != nullptr) {
            psram_allocated = true;
            return true;
        }
    }
#endif

    buffer = static_cast<uint8_t *>(malloc(buffer_size));

    if (buffer == nullptr) {
        psram_allocated = false;
        return false;
    }

    psram_allocated = false;
    return true;
}

void AudioInputBuffer::freeBuffer() {
    if (buffer == nullptr) {
        return;
    }

#if defined(ARDUINO_ARCH_ESP32)
    heap_caps_free(buffer);
#else
    free(buffer);
#endif

    buffer = nullptr;
    psram_allocated = false;
}

const uint8_t *AudioInputBuffer::acquireRead(size_t &length) const {
    length = 0;

    if (buffer == nullptr || region_a_length == 0) {
        return nullptr;
    }

    length = region_a_length;
    return buffer + region_a_start;
}

bool AudioInputBuffer::releaseRead(size_t consumed) {
    if (consumed > region_a_length) {
        return false;
    }

    if (consumed == 0) {
        return true;
    }

    region_a_start += consumed;
    region_a_length -= consumed;

    if (region_a_length == 0) {
        if (region_b_length > 0) {
            region_a_start = region_b_start;
            region_a_length = region_b_length;

            region_b_start = 0;
            region_b_length = 0;
        } else {
            region_a_start = 0;
            region_a_length = 0;

            region_b_start = 0;
            region_b_length = 0;
        }
    }

    return validate();
}

uint8_t *AudioInputBuffer::getWriteRegion(size_t &length) {
    length = 0;

    if (buffer == nullptr) {
        return nullptr;
    }

    if (region_b_length > 0) {
        const size_t end = region_b_start + region_b_length;

        if (region_a_start > end) {
            length = region_a_start - end;
            return buffer + end;
        }

        return nullptr;
    }

    const size_t end_of_a = region_a_start + region_a_length;

    const size_t free_after_a = buffer_size - end_of_a;

    const size_t free_before_a = region_a_start;

    if (region_a_length == 0) {
        length = buffer_size;
        return buffer;
    }

    if (free_after_a > 0) {
        if (free_after_a >= free_before_a) {
            length = free_after_a;
            return buffer + end_of_a;
        }
    }

    if (free_before_a > 0) {
        region_b_start = 0;

        length = free_before_a;
        return buffer;
    }

    return nullptr;
}

uint8_t *AudioInputBuffer::acquireWrite(size_t &length) {
    if (write_acquired) {
        length = 0;
        return nullptr;
    }

    uint8_t *ptr = getWriteRegion(length);

    if (ptr == nullptr || length == 0) {
        return nullptr;
    }

    write_acquired = true;

    write_start = static_cast<size_t>(ptr - buffer);

    write_length = length;

    return ptr;
}

bool AudioInputBuffer::commitWrite(size_t written) {
    if (!write_acquired) {
        return false;
    }

    if (written > write_length) {
        write_acquired = false;
        write_start = 0;
        write_length = 0;
        return false;
    }

    if (written == 0) {
        write_acquired = false;
        write_start = 0;
        write_length = 0;
        return true;
    }

    if (region_a_length == 0 && region_b_length == 0) {
        region_a_start = write_start;
        region_a_length = written;
    } else if (region_b_length > 0) {
        const size_t expected = region_b_start + region_b_length;

        if (write_start != expected) {
            write_acquired = false;
            write_start = 0;
            write_length = 0;
            return false;
        }

        region_b_length += written;
    } else {
        const size_t end_of_a = region_a_start + region_a_length;

        if (write_start == end_of_a) {
            region_a_length += written;
        } else if (write_start == 0 && region_a_start > 0) {
            region_b_start = 0;
            region_b_length = written;
        } else {
            write_acquired = false;
            write_start = 0;
            write_length = 0;
            return false;
        }
    }

    write_acquired = false;
    write_start = 0;
    write_length = 0;

    return validate();
}

AudioSourceStatus AudioInputBuffer::fill(AudioSource &source) {
    /*
     * First decide whether a refill is necessary.
     */
    const size_t current = availableRead();
    const size_t threshold = source.refillThreshold();

    if (current >= threshold) {
        return AudioSourceStatus::WOULD_BLOCK;
    }

    size_t available = 0;

    uint8_t *destination = acquireWrite(available);

    if (destination == nullptr || available == 0) {
        return AudioSourceStatus::WOULD_BLOCK;
    }

    size_t requested;

    if (source.fillToThreshold()) {
        /*
         * Streaming:
         * request exactly the missing amount up to the threshold.
         *
         * Example:
         *   31744 -> request 1024 -> 32768
         */
        requested = threshold - current;
    } else {
        /*
         * Filesystem:
         * request one complete refill chunk.
         *
         * Example:
         *   1536 -> request 2048 -> 3584
         */
        requested = source.fillSize();
    }

    requested = min(requested, available);

    if (requested == 0) {
        commitWrite(0);
        return AudioSourceStatus::WOULD_BLOCK;
    }

    size_t received = 0;

    const AudioSourceStatus status =
        source.read(destination, requested, received);

    if (status == AudioSourceStatus::DATA && received == 0) {
        commitWrite(0);
        return AudioSourceStatus::ERROR;
    }

    if (!commitWrite(received)) {
        return AudioSourceStatus::ERROR;
    }

    return status;
}

size_t AudioInputBuffer::availableRead() const {
    return region_a_length + region_b_length;
    ;
}

size_t AudioInputBuffer::availableWrite() const {
    if (buffer == nullptr) {
        return 0;
    }

    return buffer_size - region_a_length - region_b_length;
}

size_t AudioInputBuffer::capacity() const {
    return buffer_size;
}

bool AudioInputBuffer::empty() const {
    return region_a_length == 0 && region_b_length == 0;
}

bool AudioInputBuffer::full() const {
    return availableWrite() == 0;
}

bool AudioInputBuffer::usingPSRAM() const {
    return psram_allocated;
}

bool AudioInputBuffer::validate() const {
    if (buffer == nullptr) {
        return true;
    }

    if (region_a_start > buffer_size || region_b_start > buffer_size) {
        return false;
    }

    if (region_a_length > buffer_size - region_a_start) {
        return false;
    }

    if (region_b_length > buffer_size - region_b_start) {
        return false;
    }

    if (region_a_length > 0 && region_b_length > 0) {
        const size_t a_end = region_a_start + region_a_length;

        const size_t b_end = region_b_start + region_b_length;

        const bool overlap =
            (region_a_start < b_end) && (region_b_start < a_end);

        if (overlap) {
            return false;
        }
    }

    if (write_acquired) {
        if (write_start > buffer_size) {
            return false;
        }

        if (write_length > buffer_size - write_start) {
            return false;
        }
    }

    return true;
}
