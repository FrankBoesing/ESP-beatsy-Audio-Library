#include <Arduino.h>
#include <unity.h>

#include "Audio.h"

#if defined(CONFIG_IDF_TARGET_ESP32S3)
static constexpr bool IS_ESP32S3 = true;
#else
static constexpr bool IS_ESP32S3 = false;
#endif

class SineTestSource : public AudioStream
{
public:
    SineTestSource()
        : AudioStream(0, nullptr)
    {
    }

    void update() override
    {
        audio_block_t *left = allocate();
        audio_block_t *right = allocate();

        if (left == nullptr || right == nullptr) {
            if (left) release(left);
            if (right) release(right);
            return;
        }

        /*
         * 1 kHz test tone.
         *
         * This is only a hardware smoke test. The exact waveform/frequency
         * validation will be moved to a dedicated AudioSynthSine test later.
         */
        static float phase = 0.0f;
        const float fs = static_cast<float>(AudioStream::sampleRate());
        const float step = 2.0f * PI * 1000.0f / fs;

        for (size_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            const int16_t sample =
                static_cast<int16_t>(sinf(phase) * 12000.0f);

            left->data[i] = sample;
            right->data[i] = sample;

            phase += step;
            if (phase >= 2.0f * PI) {
                phase -= 2.0f * PI;
            }
        }

        transmit(left, 0);
        transmit(right, 1);

        release(left);
        release(right);
    }
};

static void test_i2s_output_smoke()
{
    AudioOutputI2S::Pins pins;

#if defined(CONFIG_IDF_TARGET_ESP32)
    /*
     * ESP32 Audio Kit V2.2 / ES8388.
     *
     * These are board-specific test pins, intentionally passed explicitly.
     * They are NOT part of the generic AudioOutputI2S defaults.
     */
    pins = {
        27, // BCLK
        25, // LRCLK / WS
        26, // DOUT
        0   // MCLK
    };

    pinMode(21, OUTPUT);
    digitalWrite(21, HIGH); // enable the Audio-Kit power amplifier
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
    pins = AudioOutputI2S::defaultPins();
#else
    TEST_IGNORE_MESSAGE(
        "No default I2S test pins defined for this ESP32 target");
    return;
#endif

    AudioOutputI2S output(pins);
    TEST_ASSERT_TRUE(output.begin());
    TEST_ASSERT_TRUE(output.isRunning());

    SineTestSource source;
    AudioConnection left(source, 0, output, 0);
    AudioConnection right(source, 1, output, 1);

    TEST_ASSERT_TRUE(source.isActive());
    TEST_ASSERT_TRUE(output.isActive());

    /*
     * The current test verifies that the AudioStream graph can continuously
     * feed the I2S TX path. It does not yet verify the acoustic output.
     *
     * For the ESP32 Audio Kit V2.2 the ES8388 still needs its I2C codec
     * configuration. That is deliberately kept outside the generic I2S
     * output and will be added as the board/codec layer.
     */
    delay(3000);

    output.end();

    TEST_ASSERT_FALSE(output.isRunning());
}

void setup()
{
    delay(1000);

    AudioMemory(16);

    UNITY_BEGIN();
    RUN_TEST(test_i2s_output_smoke);
    UNITY_END();
}

void loop()
{
}
