#include <Arduino.h>
#include <unity.h>

#include "control_es8388.h"

static AudioControlES8388 codec;

void test_es8388_control()
{
    TEST_ASSERT_TRUE_MESSAGE(
        codec.enable(),
        "ES8388 not found or codec initialization failed");

    TEST_ASSERT_TRUE(codec.isConnected());

    // Keep the first hardware test conservative.
    TEST_ASSERT_TRUE(codec.volume(0.25f));
    TEST_ASSERT_TRUE(codec.unmute());

    delay(100);

    TEST_ASSERT_TRUE(codec.mute());
    TEST_ASSERT_TRUE(codec.disable());
}

void setup()
{
    delay(1000);
    UNITY_BEGIN();
    RUN_TEST(test_es8388_control);
    UNITY_END();
}

void loop()
{
}
