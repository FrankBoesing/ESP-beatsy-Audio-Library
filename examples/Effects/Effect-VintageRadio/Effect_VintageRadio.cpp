#include <Arduino.h>
#include <SD_MMC.h>
#include <Audio.h>
#include <math.h>

// Compare the same MP3 stream through several processing paths.
// Copy a test file to the SD card as /test.mp3.
//
// Serial commands:
//   0 = original signal
//   1 = vintage-radio band limiting
//   2 = band limiting + more prominent mids
//   3 = band limiting + soft saturation
//   4 = band limiting + mids + soft saturation
//   n = next mode, ? = show this help

// ============================================================================
// Hardware
// ============================================================================

AudioOutputI2S output({
    PIN_I2S_BLCK, // BCLK
    PIN_I2S_WS,   // WS / LRCLK
    PIN_I2S_DOUT, // DOUT
    PIN_I2S_MLCK  // MCLK
});

#if defined(AUDIO_CODEC_ES8388)
AudioControlES8388 codec({PIN_I2C_SDA, PIN_I2C_SCL, PIN_AMPLIFIER, PIN_AMPLIFIER_ACTIVE});
#elif defined(AUDIO_CODEC_ES8311)
AudioControlES8311 codec({PIN_I2C_SDA, PIN_I2C_SCL, PIN_AMPLIFIER, PIN_AMPLIFIER_ACTIVE});
#else
#error "AUDIO_CODEC not defined"
#endif

AudioSourceFile source;
AudioPlayMp3 mp3;

// ============================================================================
// Parallel processing paths
// ============================================================================

// Radio bandwidth: two cascaded biquads per channel.
AudioFilterBiquad radioFilterL;
AudioFilterBiquad radioFilterR;

// A broad mid-forward profile, applied after the radio band limiting.
AudioFilterBiquad midProfileL;
AudioFilterBiquad midProfileR;

// Independent saturation paths allow immediate A/B comparisons.
AudioEffectWaveshaper saturationL;
AudioEffectWaveshaper saturationR;
AudioEffectWaveshaper midSaturationL;
AudioEffectWaveshaper midSaturationR;

// Select one of five paths without stopping or restarting playback.
AudioMixerN selectL(5);
AudioMixerN selectR(5);

// ============================================================================
// Connections
// ============================================================================

// Original / dry signal.
AudioConnection dryL(mp3, 0, selectL, 0);
AudioConnection dryR(mp3, 1, selectR, 0);

// Radio band limiting.
AudioConnection radioInputL(mp3, 0, radioFilterL, 0);
AudioConnection radioInputR(mp3, 1, radioFilterR, 0);
AudioConnection radioOnlyL(radioFilterL, 0, selectL, 1);
AudioConnection radioOnlyR(radioFilterR, 0, selectR, 1);

// Radio + more prominent mids.
AudioConnection midInputL(radioFilterL, 0, midProfileL, 0);
AudioConnection midInputR(radioFilterR, 0, midProfileR, 0);
AudioConnection midOnlyL(midProfileL, 0, selectL, 2);
AudioConnection midOnlyR(midProfileR, 0, selectR, 2);

// Radio + saturation.
AudioConnection saturationInputL(radioFilterL, 0, saturationL, 0);
AudioConnection saturationInputR(radioFilterR, 0, saturationR, 0);
AudioConnection saturationOnlyL(saturationL, 0, selectL, 3);
AudioConnection saturationOnlyR(saturationR, 0, selectR, 3);

// Radio + mids + saturation.
AudioConnection midSaturationInputL(midProfileL, 0, midSaturationL, 0);
AudioConnection midSaturationInputR(midProfileR, 0, midSaturationR, 0);
AudioConnection midSaturationOnlyL(midSaturationL, 0, selectL, 4);
AudioConnection midSaturationOnlyR(midSaturationR, 0, selectR, 4);

// Final stereo output.
AudioConnection outputL(selectL, 0, output, 0);
AudioConnection outputR(selectR, 0, output, 1);

// ============================================================================
// Mode selection
// ============================================================================

static constexpr uint8_t MODE_COUNT = 5;
static constexpr float SATURATION_LEVEL = 0.78f;
static uint8_t currentMode = 0;

static const char *const MODE_NAMES[MODE_COUNT] = {
    "Original",
    "Vintage radio: highpass + lowpass",
    "Vintage radio + mid emphasis",
    "Vintage radio + soft saturation",
    "Vintage radio + mids + soft saturation"
};

static void printHelp() {
    Serial.println();
    Serial.println("Vintage Radio A/B test");
    for (uint8_t i = 0; i < MODE_COUNT; ++i) {
        Serial.printf("  %u = %s\n", i, MODE_NAMES[i]);
    }
    Serial.println("  n = next mode, ? = help");
    Serial.println("Switching does not restart playback.");
    Serial.println();
}

static void selectMode(uint8_t mode) {
    if (mode >= MODE_COUNT) {
        return;
    }

    // Keep the two stereo selectors consistent across an audio update.
    {
        audio_detail::GraphUpdateLock lock;

        for (uint8_t channel = 0; channel < MODE_COUNT; ++channel) {
            const float gain = (channel == mode)
                                   ? ((mode >= 3) ? SATURATION_LEVEL : 1.0f)
                                   : 0.0f;
            selectL.gain(channel, gain);
            selectR.gain(channel, gain);
        }

        currentMode = mode;
    }

    Serial.printf("Mode %u: %s\n", currentMode, MODE_NAMES[currentMode]);
}

static void configureSaturation() {
    // Gentle, symmetric soft clipping. The Waveshaper copies the curve into
    // its own storage, so this temporary array can live on the setup stack.
    constexpr int CURVE_SIZE = 257; // 2^N + 1
    constexpr float DRIVE = 1.4f;
    const float normalizer = tanhf(DRIVE);
    float curve[CURVE_SIZE];

    for (int i = 0; i < CURVE_SIZE; ++i) {
        const float x = (2.0f * (float)i / (float)(CURVE_SIZE - 1)) - 1.0f;
        curve[i] = tanhf(DRIVE * x) / normalizer;
    }

    saturationL.shape(curve, CURVE_SIZE);
    saturationR.shape(curve, CURVE_SIZE);
    midSaturationL.shape(curve, CURVE_SIZE);
    midSaturationR.shape(curve, CURVE_SIZE);
}

// ============================================================================
// Setup
// ============================================================================

void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println();
    Serial.println("======================================");
    Serial.println(" Vintage Radio A/B test");
    Serial.println(" SD_MMC file: /test.mp3");
    Serial.println("======================================");

    // AudioMixerN starts with all gains at 1. Mute its inputs before starting
    // the audio scheduler, then select the initial path after initialization.
    for (uint8_t channel = 0; channel < MODE_COUNT; ++channel) {
        selectL.gain(channel, 0.0f);
        selectR.gain(channel, 0.0f);
    }

    AudioMemory(32);

    if (!codec.enable()) {
        Serial.println("ERROR: Codec initialization failed");
        while (true) {
            delay(1000);
        }
    }
    codec.volume(0.65f);

    // Vintage-radio starting profile. Frequencies are tuning parameters,
    // not a claim to reproduce one particular historical radio.
    radioFilterL.setHighpass(0, 150.0f);
    radioFilterL.setLowpass(1, 3800.0f);
    radioFilterR.setHighpass(0, 150.0f);
    radioFilterR.setLowpass(1, 3800.0f);

    // Gently reduce the low and high shelves to make the midrange more
    // prominent. This is deliberately separate from the basic radio filter.
    midProfileL.setLowShelf(0, 350.0f, -2.0f);
    midProfileL.setHighShelf(1, 1800.0f, -3.0f);
    midProfileR.setLowShelf(0, 350.0f, -2.0f);
    midProfileR.setHighShelf(1, 1800.0f, -3.0f);

    configureSaturation();
    selectMode(1);

    if (!SD_MMC.begin("/sdcard", true)) {
        Serial.println("ERROR: SD_MMC initialization failed");
        while (true) {
            delay(1000);
        }
    }

    if (!source.open(SD_MMC, "/test.mp3")) {
        Serial.println("ERROR: Could not open /test.mp3");
        while (true) {
            delay(1000);
        }
    }

    if (!mp3.play(source)) {
        Serial.println("ERROR: MP3 playback could not be started");
        while (true) {
            delay(1000);
        }
    }

    Serial.println("MP3 playback started.");
    printHelp();
}

// ============================================================================
// Loop
// ============================================================================

void loop() {
    while (Serial.available() > 0) {
        const char c = (char)Serial.read();

        if (c >= '0' && c < ('0' + MODE_COUNT)) {
            selectMode((uint8_t)(c - '0'));
        } else if (c == 'n' || c == 'N') {
            selectMode((uint8_t)((currentMode + 1U) % MODE_COUNT));
        } else if (c == '?') {
            printHelp();
        }
    }

    delay(10);
}
