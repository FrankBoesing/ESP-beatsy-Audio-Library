#include <Arduino.h>
#include <unity.h>

#include "softcodecs/AudioInputBuffer.h"
#include "softcodecs/AudioSource.h"

static constexpr size_t BUFFER_SIZE = 64 * 1024;
static constexpr size_t FILE_FILL_SIZE = 2 * 1024;
static constexpr size_t STREAM_FILL_SIZE = 32 * 1024;


/* -------------------------------------------------------------------------- */
/* Fake file source                                                           */
/* -------------------------------------------------------------------------- */

class FakeFileSource : public AudioSource
{
public:
    explicit FakeFileSource(size_t totalBytes)
        : _total(totalBytes),
          _position(0),
          _readCalls(0),
          _lastRequested(0)
    {
    }

    AudioSourceStatus read(
        uint8_t *buffer,
        size_t requested,
        size_t &received
    ) override
    {
        ++_readCalls;
        _lastRequested = requested;

        if (_position >= _total) {
            received = 0;
            return AudioSourceStatus::END_OF_STREAM;
        }

        const size_t remaining = _total - _position;
        received = min(requested, remaining);

        for (size_t i = 0; i < received; ++i) {
            buffer[i] =
                static_cast<uint8_t>(
                    (_position + i) & 0xFF
                );
        }

        _position += received;

        return AudioSourceStatus::DATA;
    }

    uint64_t position() const override
    {
        return _position;
    }

    uint64_t size() const override
    {
        return _total;
    }

    bool isSeekable() const override
    {
        return true;
    }

    bool seek(uint64_t position) override
    {
        if (position > _total) {
            return false;
        }

        _position = position;
        return true;
    }

    void close() override
    {
    }

    bool isOpen() const override
    {
        return true;
    }

    size_t refillThreshold() const override
    {
        return FILE_FILL_SIZE;
    }

    size_t fillSize() const override
    {
        return FILE_FILL_SIZE;
    }

    bool fillToThreshold() const override
    {
        return false;
    }

    size_t readCalls() const
    {
        return _readCalls;
    }

    size_t lastRequested() const
    {
        return _lastRequested;
    }

private:
    size_t _total;
    size_t _position;
    size_t _readCalls;
    size_t _lastRequested;
};


/* -------------------------------------------------------------------------- */
/* Fake stream source                                                         */
/* -------------------------------------------------------------------------- */

class FakeStreamSource : public AudioSource
{
public:
    explicit FakeStreamSource(size_t availableBytes = 0)
        : _available(availableBytes),
          _position(0),
          _readCalls(0),
          _lastRequested(0),
          _block(false)
    {
    }

    AudioSourceStatus read(
        uint8_t *buffer,
        size_t requested,
        size_t &received
    ) override
    {
        ++_readCalls;
        _lastRequested = requested;
        received = 0;

        if (_block) {
            return AudioSourceStatus::WOULD_BLOCK;
        }

        if (_available == 0) {
            return AudioSourceStatus::WOULD_BLOCK;
        }

        const size_t n = min(requested, _available);

        for (size_t i = 0; i < n; ++i) {
            buffer[i] =
                static_cast<uint8_t>(
                    (_position + i + 1) & 0xFF
                );
        }

        _position += n;
        _available -= n;
        received = n;

        return AudioSourceStatus::DATA;
    }

    uint64_t position() const override
    {
        return _position;
    }

    uint64_t size() const override
    {
        return 0;
    }

    bool isSeekable() const override
    {
        return false;
    }

    bool seek(uint64_t position) override
    {
        (void)position;
        return false;
    }

    void close() override
    {
    }

    bool isOpen() const override
    {
        return true;
    }

    size_t refillThreshold() const override
    {
        return STREAM_FILL_SIZE;
    }

    size_t fillSize() const override
    {
        return STREAM_FILL_SIZE;
    }

    bool fillToThreshold() const override
    {
        return true;
    }

    void addData(size_t bytes)
    {
        _available += bytes;
        _block = false;
    }

    void setWouldBlock(bool block)
    {
        _block = block;
    }

    size_t readCalls() const
    {
        return _readCalls;
    }

    size_t lastRequested() const
    {
        return _lastRequested;
    }

private:
    size_t _available;
    size_t _position;
    size_t _readCalls;
    size_t _lastRequested;
    bool _block;
};


/* -------------------------------------------------------------------------- */
/* Test state                                                                 */
/* -------------------------------------------------------------------------- */

static AudioInputBuffer *buffer = nullptr;
static FakeFileSource *fileSource = nullptr;
static FakeStreamSource *streamSource = nullptr;

void setUp()
{
}

void tearDown()
{
}


/* -------------------------------------------------------------------------- */
/* File tests                                                                 */
/* -------------------------------------------------------------------------- */

static void test_file_policy_is_2kb()
{
    TEST_ASSERT_EQUAL_UINT32(
        FILE_FILL_SIZE,
        fileSource->refillThreshold()
    );

    TEST_ASSERT_EQUAL_UINT32(
        FILE_FILL_SIZE,
        fileSource->fillSize()
    );

    TEST_ASSERT_FALSE(
        fileSource->fillToThreshold()
    );
}

static void test_file_initial_fill_is_2kb()
{
    const AudioSourceStatus status =
        buffer->fill(*fileSource);

    TEST_ASSERT_EQUAL(
        AudioSourceStatus::DATA,
        status
    );

    TEST_ASSERT_EQUAL_UINT32(
        FILE_FILL_SIZE,
        buffer->availableRead()
    );

    TEST_ASSERT_EQUAL_UINT32(
        FILE_FILL_SIZE,
        fileSource->lastRequested()
    );
}

static void test_file_does_not_fill_at_2kb()
{
    const size_t before =
        buffer->availableRead();

    const size_t callsBefore =
        fileSource->readCalls();

    const AudioSourceStatus status =
        buffer->fill(*fileSource);

    TEST_ASSERT_EQUAL(
        AudioSourceStatus::WOULD_BLOCK,
        status
    );

    TEST_ASSERT_EQUAL_UINT32(
        before,
        buffer->availableRead()
    );

    TEST_ASSERT_EQUAL_UINT32(
        callsBefore,
        fileSource->readCalls()
    );
}

static void test_file_refill_stays_below_4kb()
{
    TEST_ASSERT_TRUE(
        buffer->releaseRead(512)
    );

    TEST_ASSERT_EQUAL_UINT32(
        1536,
        buffer->availableRead()
    );

    const AudioSourceStatus status =
        buffer->fill(*fileSource);

    TEST_ASSERT_EQUAL(
        AudioSourceStatus::DATA,
        status
    );

    TEST_ASSERT_EQUAL_UINT32(
        3584,
        buffer->availableRead()
    );

    TEST_ASSERT_LESS_THAN_UINT32(
        4096,
        buffer->availableRead()
    );

    TEST_ASSERT_EQUAL_UINT32(
        FILE_FILL_SIZE,
        fileSource->lastRequested()
    );
}


/* -------------------------------------------------------------------------- */
/* Stream tests                                                               */
/* -------------------------------------------------------------------------- */

static void test_stream_policy_is_32kb()
{
    TEST_ASSERT_EQUAL_UINT32(
        STREAM_FILL_SIZE,
        streamSource->refillThreshold()
    );

    TEST_ASSERT_EQUAL_UINT32(
        STREAM_FILL_SIZE,
        streamSource->fillSize()
    );

    TEST_ASSERT_TRUE(
        streamSource->fillToThreshold()
    );
}

static void test_stream_initial_prefill_is_32kb()
{
    const AudioSourceStatus status =
        buffer->fill(*streamSource);

    TEST_ASSERT_EQUAL(
        AudioSourceStatus::DATA,
        status
    );

    TEST_ASSERT_EQUAL_UINT32(
        STREAM_FILL_SIZE,
        buffer->availableRead()
    );

    TEST_ASSERT_EQUAL_UINT32(
        STREAM_FILL_SIZE,
        streamSource->lastRequested()
    );
}

static void test_stream_does_not_fill_at_32kb()
{
    const size_t before =
        buffer->availableRead();

    const size_t callsBefore =
        streamSource->readCalls();

    const AudioSourceStatus status =
        buffer->fill(*streamSource);

    TEST_ASSERT_EQUAL(
        AudioSourceStatus::WOULD_BLOCK,
        status
    );

    TEST_ASSERT_EQUAL_UINT32(
        before,
        buffer->availableRead()
    );

    TEST_ASSERT_EQUAL_UINT32(
        callsBefore,
        streamSource->readCalls()
    );
}

static void test_stream_refills_from_31kb_to_32kb()
{
    TEST_ASSERT_TRUE(
        buffer->releaseRead(1024)
    );

    TEST_ASSERT_EQUAL_UINT32(
        31744,
        buffer->availableRead()
    );

    streamSource->addData(1024);

    const AudioSourceStatus status =
        buffer->fill(*streamSource);

    TEST_ASSERT_EQUAL(
        AudioSourceStatus::DATA,
        status
    );

    TEST_ASSERT_EQUAL_UINT32(
        STREAM_FILL_SIZE,
        buffer->availableRead()
    );

    TEST_ASSERT_EQUAL_UINT32(
        1024,
        streamSource->lastRequested()
    );
}

static void test_stream_would_block_preserves_buffer()
{
    TEST_ASSERT_TRUE(
        buffer->releaseRead(512)
    );

    const size_t before =
        buffer->availableRead();

    streamSource->setWouldBlock(true);

    const size_t callsBefore =
        streamSource->readCalls();

    const AudioSourceStatus status =
        buffer->fill(*streamSource);

    TEST_ASSERT_EQUAL(
        AudioSourceStatus::WOULD_BLOCK,
        status
    );

    TEST_ASSERT_EQUAL_UINT32(
        before,
        buffer->availableRead()
    );

    TEST_ASSERT_EQUAL_UINT32(
        callsBefore + 1,
        streamSource->readCalls()
    );
}

static void test_stream_real_data_available()
{
    size_t length = 0;

    const uint8_t *data =
        buffer->acquireRead(length);

    TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT_GREATER_THAN_UINT32(0, length);

    bool nonZeroFound = false;

    for (size_t i = 0; i < length; ++i) {
        if (data[i] != 0) {
            nonZeroFound = true;
            break;
        }
    }

    TEST_ASSERT_TRUE(nonZeroFound);
}


/* -------------------------------------------------------------------------- */
/* Setup                                                                      */
/* -------------------------------------------------------------------------- */

void setup()
{
    delay(200);

    UNITY_BEGIN();

    buffer =
        new AudioInputBuffer(BUFFER_SIZE);

    fileSource =
        new FakeFileSource(64 * 1024);

    streamSource =
        new FakeStreamSource(STREAM_FILL_SIZE);

    TEST_ASSERT_NOT_NULL(buffer);
    TEST_ASSERT_NOT_NULL(fileSource);
    TEST_ASSERT_NOT_NULL(streamSource);

    TEST_ASSERT_TRUE(
        buffer->begin()
    );

    RUN_TEST(test_file_policy_is_2kb);
    RUN_TEST(test_file_initial_fill_is_2kb);
    RUN_TEST(test_file_does_not_fill_at_2kb);
    RUN_TEST(test_file_refill_stays_below_4kb);

    /*
     * reset() is intentionally public in this working version.
     * It clears the logical buffer without reallocating memory.
     */
    buffer->reset();

    RUN_TEST(test_stream_policy_is_32kb);
    RUN_TEST(test_stream_initial_prefill_is_32kb);
    RUN_TEST(test_stream_does_not_fill_at_32kb);
    RUN_TEST(test_stream_refills_from_31kb_to_32kb);
    RUN_TEST(test_stream_would_block_preserves_buffer);
    RUN_TEST(test_stream_real_data_available);

    buffer->end();

    delete streamSource;
    delete fileSource;
    delete buffer;

    streamSource = nullptr;
    fileSource = nullptr;
    buffer = nullptr;

    UNITY_END();
}

void loop()
{
    delay(1000);
}
