#include <Arduino.h>
#include <unity.h>

#include "softcodecs/AudioDecoderStream.h"

namespace {

class TestDecoderStream : public AudioDecoderStream {
  public:
    TestDecoderStream() : AudioDecoderStream(32) {}

    size_t decodeCalls() const {
        return _decodeCalls;
    }

  protected:
    DecodeResult decodePcmBuffer(int16_t *destination, size_t capacity,
                                 size_t &outSamples) override {
        ++_decodeCalls;

        if (destination == nullptr || capacity < 8) {
            outSamples = 0;
            return DecodeResult::ERROR;
        }

        /*
         * One stereo-interleaved test pattern:
         * L0, R0, L1, R1, ...
         */
        destination[0] = 100;
        destination[1] = -100;
        destination[2] = 200;
        destination[3] = -200;
        destination[4] = 300;
        destination[5] = -300;
        destination[6] = 400;
        destination[7] = -400;

        outSamples = 8;

        return DecodeResult::END_OF_STREAM;
    }

  private:
    size_t _decodeCalls = 0;
};

void test_constructor_does_not_start_decoder_task() {
    TestDecoderStream decoder;

    TEST_ASSERT_FALSE(decoder.decoderTaskRunning());

    TEST_ASSERT_FALSE(decoder.decoderFinished());

    TEST_ASSERT_TRUE(decoder.samplesPlayed() == 0);
}

void test_decoder_task_can_be_started_and_stopped() {
    TestDecoderStream decoder;

    TEST_ASSERT_TRUE(decoder.startDecoderTask());

    /*
     * Give the task one scheduler opportunity.
     */
    delay(10);

    TEST_ASSERT_TRUE(decoder.decoderFinished());

    TEST_ASSERT_FALSE(decoder.decoderTaskRunning());

    decoder.stopDecoderTask();

    TEST_ASSERT_FALSE(decoder.decoderTaskRunning());
}

} // namespace

void setup() {
    delay(100);

    UNITY_BEGIN();

    RUN_TEST(test_constructor_does_not_start_decoder_task);

    RUN_TEST(test_decoder_task_can_be_started_and_stopped);

    UNITY_END();
}

void loop() {}
