#include <Arduino.h>
#include <AudioStream.h>
#include <unity.h>


// ============================================================================
// Test stream
// ============================================================================

class TestStream : public AudioStream
{
public:

    explicit TestStream(unsigned char inputs = 1)
        : AudioStream(inputs, queue)
    {
    }

    void update(void) override
    {
        ++updateCount;
    }

    audio_block_t *alloc()
    {
        return allocate();
    }

    void freeBlock(audio_block_t *block)
    {
        release(block);
    }

    void send(audio_block_t *block, unsigned char output = 0)
    {
        transmit(block, output);
    }

    audio_block_t *receiveRO(unsigned int input = 0)
    {
        return receiveReadOnly(input);
    }

    audio_block_t *receiveRW(unsigned int input = 0)
    {
        return receiveWritable(input);
    }

    static void runUpdatesNow()
    {
        process_all_now();
    }

    static bool startScheduler()
    {
        return update_setup();
    }

    static void stopScheduler()
    {
        update_stop();
    }

    static bool setRate(float rate)
    {
        return setSampleRate(rate);
    }

    static float getRate()
    {
        return sampleRate();
    }

    static uint32_t getBlockPeriodUs()
    {
        return blockPeriodUs();
    }

    uint32_t updateCount = 0;

private:

    audio_block_t *queue[8] = {};
};


// ============================================================================
// Static test objects
// ============================================================================

static TestStream source;
static TestStream destination1;
static TestStream destination2;


// ============================================================================
// One complete AudioStream test
// ============================================================================

void test_audio_stream_core()
{
    // Stop the automatic software clock for the deterministic core tests.
    TestStream::stopScheduler();

    // ------------------------------------------------------------------------
    // 1. allocate()
    // ------------------------------------------------------------------------

    audio_block_t *block = source.alloc();

    TEST_ASSERT_NOT_NULL(block);

    if (block == nullptr) {
        return;
    }

    TEST_ASSERT_EQUAL_UINT8(
        1,
        block->ref_count
    );

    TEST_ASSERT_EQUAL_UINT16(
        1,
        AudioStream::memoryUsage()
    );


    // ------------------------------------------------------------------------
    // 2. release()
    // ------------------------------------------------------------------------

    source.freeBlock(block);

    TEST_ASSERT_EQUAL_UINT16(
        0,
        AudioStream::memoryUsage()
    );


    // ------------------------------------------------------------------------
    // 3. Pool exhaustion
    // ------------------------------------------------------------------------

    audio_block_t *blocks[8] = {};

    for (unsigned int i = 0; i < 8; ++i) {

        blocks[i] = source.alloc();

        TEST_ASSERT_NOT_NULL(blocks[i]);

        if (blocks[i] == nullptr) {
            return;
        }
    }

    TEST_ASSERT_EQUAL_UINT16(
        8,
        AudioStream::memoryUsage()
    );

    audio_block_t *extra = source.alloc();

    TEST_ASSERT_NULL(extra);

    for (unsigned int i = 0; i < 8; ++i) {
        source.freeBlock(blocks[i]);
    }

    TEST_ASSERT_EQUAL_UINT16(
        0,
        AudioStream::memoryUsage()
    );


    // ------------------------------------------------------------------------
    // 4. Single transmit
    // ------------------------------------------------------------------------

    {
        AudioConnection connection(
            source,
            destination1
        );

        block = source.alloc();

        TEST_ASSERT_NOT_NULL(block);

        if (block == nullptr) {
            return;
        }

        block->data[0] = 1234;

        source.send(block);

        // source + destination
        TEST_ASSERT_EQUAL_UINT8(
            2,
            block->ref_count
        );

        audio_block_t *received =
            destination1.receiveRO();

        TEST_ASSERT_EQUAL_PTR(
            block,
            received
        );

        if (received != nullptr) {

            TEST_ASSERT_EQUAL_INT16(
                1234,
                received->data[0]
            );
        }

        source.freeBlock(block);

        TEST_ASSERT_EQUAL_UINT8(
            1,
            block->ref_count
        );

        destination1.freeBlock(received);

        TEST_ASSERT_EQUAL_UINT16(
            0,
            AudioStream::memoryUsage()
        );
    }


    // ------------------------------------------------------------------------
    // 5. Fan-out / reference counting
    // ------------------------------------------------------------------------

    {
        AudioConnection connection1(
            source,
            destination1
        );

        AudioConnection connection2(
            source,
            destination2
        );

        block = source.alloc();

        TEST_ASSERT_NOT_NULL(block);

        if (block == nullptr) {
            return;
        }

        block->data[0] = 555;

        source.send(block);

        // source + destination1 + destination2
        TEST_ASSERT_EQUAL_UINT8(
            3,
            block->ref_count
        );

        audio_block_t *block1 =
            destination1.receiveRO();

        audio_block_t *block2 =
            destination2.receiveRO();

        TEST_ASSERT_EQUAL_PTR(
            block,
            block1
        );

        TEST_ASSERT_EQUAL_PTR(
            block,
            block2
        );

        source.freeBlock(block);

        TEST_ASSERT_EQUAL_UINT8(
            2,
            block->ref_count
        );

        destination1.freeBlock(block1);

        TEST_ASSERT_EQUAL_UINT8(
            1,
            block->ref_count
        );

        destination2.freeBlock(block2);

        TEST_ASSERT_EQUAL_UINT16(
            0,
            AudioStream::memoryUsage()
        );
    }


    // ------------------------------------------------------------------------
    // 6. receiveWritable() with shared block
    //    -> Copy-on-Write
    // ------------------------------------------------------------------------

    {
        AudioConnection connection1(
            source,
            destination1
        );

        AudioConnection connection2(
            source,
            destination2
        );

        block = source.alloc();

        TEST_ASSERT_NOT_NULL(block);

        if (block == nullptr) {
            return;
        }

        block->data[0] = 100;

        source.send(block);

        // Remove source ownership.
        source.freeBlock(block);

        TEST_ASSERT_EQUAL_UINT8(
            2,
            block->ref_count
        );

        audio_block_t *copy =
            destination1.receiveRW();

        TEST_ASSERT_NOT_NULL(copy);

        if (copy == nullptr) {
            return;
        }

        // Shared block must be copied.
        TEST_ASSERT_NOT_EQUAL(
            block,
            copy
        );

        TEST_ASSERT_EQUAL_INT16(
            100,
            copy->data[0]
        );

        copy->data[0] = 200;

        TEST_ASSERT_EQUAL_INT16(
            200,
            copy->data[0]
        );

        // Original must remain unchanged.
        TEST_ASSERT_EQUAL_INT16(
            100,
            block->data[0]
        );

        TEST_ASSERT_EQUAL_UINT8(
            1,
            block->ref_count
        );

        audio_block_t *original =
            destination2.receiveRO();

        TEST_ASSERT_EQUAL_PTR(
            block,
            original
        );

        destination1.freeBlock(copy);
        destination2.freeBlock(original);

        TEST_ASSERT_EQUAL_UINT16(
            0,
            AudioStream::memoryUsage()
        );
    }


    // ------------------------------------------------------------------------
    // 7. receiveWritable() with unique block
    // ------------------------------------------------------------------------

    {
        AudioConnection connection(
            source,
            destination1
        );

        block = source.alloc();

        TEST_ASSERT_NOT_NULL(block);

        if (block == nullptr) {
            return;
        }

        block->data[0] = 10;

        source.send(block);

        // Destination is now sole owner.
        source.freeBlock(block);

        TEST_ASSERT_EQUAL_UINT8(
            1,
            block->ref_count
        );

        audio_block_t *received =
            destination1.receiveRW();

        TEST_ASSERT_EQUAL_PTR(
            block,
            received
        );

        if (received != nullptr) {

            received->data[0] = 20;

            TEST_ASSERT_EQUAL_INT16(
                20,
                received->data[0]
            );

            destination1.freeBlock(received);
        }

        TEST_ASSERT_EQUAL_UINT16(
            0,
            AudioStream::memoryUsage()
        );
    }


    // ------------------------------------------------------------------------
    // 8. Invalid destination input / connection collision
    // ------------------------------------------------------------------------

    {
        AudioConnection connection1;

        int result1 =
            connection1.connect(
                source,
                destination1
            );

        TEST_ASSERT_EQUAL_INT(
            0,
            result1
        );

        AudioConnection connection2;

        int result2 =
            connection2.connect(
                destination2,
                destination1
            );

        // destination1 input 0 is already occupied.
        TEST_ASSERT_EQUAL_INT(
            4,
            result2
        );
    }


    // ------------------------------------------------------------------------
    // 9. disconnect() with pending block
    // ------------------------------------------------------------------------

    {
        AudioConnection connection(
            source,
            destination1
        );

        block = source.alloc();

        TEST_ASSERT_NOT_NULL(block);

        if (block == nullptr) {
            return;
        }

        source.send(block);

        TEST_ASSERT_EQUAL_UINT8(
            2,
            block->ref_count
        );

        TEST_ASSERT_EQUAL_UINT16(
            1,
            AudioStream::memoryUsage()
        );

        int result =
            connection.disconnect();

        TEST_ASSERT_EQUAL_INT(
            0,
            result
        );

        // Disconnect must release destination ownership.
        TEST_ASSERT_EQUAL_UINT8(
            1,
            block->ref_count
        );

        source.freeBlock(block);

        TEST_ASSERT_EQUAL_UINT16(
            0,
            AudioStream::memoryUsage()
        );
    }


    // ------------------------------------------------------------------------
    // 10. Active state
    // ------------------------------------------------------------------------

    TEST_ASSERT_FALSE(
        source.isActive()
    );

    TEST_ASSERT_FALSE(
        destination1.isActive()
    );

    {
        AudioConnection connection(
            source,
            destination1
        );

        TEST_ASSERT_TRUE(
            source.isActive()
        );

        TEST_ASSERT_TRUE(
            destination1.isActive()
        );

        int result =
            connection.disconnect();

        TEST_ASSERT_EQUAL_INT(
            0,
            result
        );
    }

    TEST_ASSERT_FALSE(
        source.isActive()
    );

    TEST_ASSERT_FALSE(
        destination1.isActive()
    );


    // ------------------------------------------------------------------------
    // 11. update_all()
    // ------------------------------------------------------------------------

    source.updateCount = 0;
    destination1.updateCount = 0;
    destination2.updateCount = 0;

    {
        AudioConnection connection(
            source,
            destination1
        );

        TestStream::runUpdatesNow();

        TEST_ASSERT_EQUAL_UINT32(
            1,
            source.updateCount
        );

        TEST_ASSERT_EQUAL_UINT32(
            1,
            destination1.updateCount
        );

        TEST_ASSERT_EQUAL_UINT32(
            0,
            destination2.updateCount
        );
    }


    // ------------------------------------------------------------------------
    // 12. Software scheduler and sample-rate timing
    // ------------------------------------------------------------------------

    TEST_ASSERT_TRUE(TestStream::setRate(44100.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 44100.0f, TestStream::getRate());
    TEST_ASSERT_EQUAL_UINT32(2902, TestStream::getBlockPeriodUs());

    source.updateCount = 0;
    destination1.updateCount = 0;

    {
        AudioConnection schedulerConnection(
            source,
            destination1
        );

        TEST_ASSERT_TRUE(TestStream::startScheduler());

        const uint32_t start = millis();
        while (millis() - start < 25) {
            delay(1);
        }

        TEST_ASSERT_GREATER_THAN_UINT32(0, source.updateCount);
        TEST_ASSERT_GREATER_THAN_UINT32(0, destination1.updateCount);

        TestStream::stopScheduler();

        const uint32_t stoppedCount = source.updateCount;
        delay(10);
        TEST_ASSERT_EQUAL_UINT32(stoppedCount, source.updateCount);

        // Verify that a changed sample rate changes the software block period.
        TEST_ASSERT_TRUE(TestStream::setRate(48000.0f));
        TEST_ASSERT_EQUAL_UINT32(2667, TestStream::getBlockPeriodUs());

        TEST_ASSERT_TRUE(TestStream::startScheduler());
        delay(10);
        TEST_ASSERT_GREATER_THAN_UINT32(stoppedCount, source.updateCount);
        TestStream::stopScheduler();
    }

    // Restore the project default for subsequent tests.
    TEST_ASSERT_TRUE(TestStream::setRate(44100.0f));

    // ------------------------------------------------------------------------
    // 13. Maximum memory usage
    // ------------------------------------------------------------------------

    AudioStream::memoryUsageMaxReset();

    audio_block_t *a = source.alloc();
    audio_block_t *b = source.alloc();
    audio_block_t *c = source.alloc();

    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_NOT_NULL(c);

    if (a == nullptr || b == nullptr || c == nullptr) {

        if (a != nullptr) source.freeBlock(a);
        if (b != nullptr) source.freeBlock(b);
        if (c != nullptr) source.freeBlock(c);

        return;
    }

    TEST_ASSERT_EQUAL_UINT16(
        3,
        AudioStream::memoryUsage()
    );

    TEST_ASSERT_EQUAL_UINT16(
        3,
        AudioStream::memoryUsageMax()
    );

    source.freeBlock(a);
    source.freeBlock(b);
    source.freeBlock(c);

    TEST_ASSERT_EQUAL_UINT16(
        0,
        AudioStream::memoryUsage()
    );
}


// ============================================================================
// Arduino / Unity
// ============================================================================

void setup()
{
    delay(2000);

    AudioMemory(8);

    UNITY_BEGIN();

    RUN_TEST(test_audio_stream_core);

    UNITY_END();
}


void loop()
{
}
