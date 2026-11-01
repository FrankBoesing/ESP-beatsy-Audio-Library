/*
 * AudioSynthWavetableV2 demonstration.
 *
 * Demonstrates polyphonic playback, key/velocity zones and two sample layers
 * for notes 48..84 at velocities 80..127.
 */

#include <Audio.h>

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

AudioSynthWavetableV2 synth;
AudioConnection patchLeft(synth, 0, output, 0);
AudioConnection patchRight(synth, 0, output, 1);

// Each table contains 32 playable frames plus one interpolation guard frame.
// The final entry repeats the first entry so the table wraps without a click.
static const int16_t sineWave[33] = {
     0, 6393, 12539, 18204, 23170, 27245, 30273, 32137,
     32767, 32137, 30273, 27245, 23170, 18204, 12539, 6393,
     0, -6393, -12539, -18204, -23170, -27245, -30273, -32137,
     -32767, -32137, -30273, -27245, -23170, -18204, -12539, -6393,
     0
};

static const int16_t sawWave[33] = {
    -32768, -30720, -28672, -26624, -24576, -22528, -20480, -18432,
    -16384, -14336, -12288, -10240, -8192, -6144, -4096, -2048,
    0, 2047, 4095, 6143, 8191, 10239, 12287, 14335,
    16383, 18431, 20479, 22527, 24575, 26623, 28671, 30719,
    -32768
};

static const AudioSynthWavetableV2::sample_data sineSample(
    sineWave, 33, 5, 4294967296.0f / AUDIO_SAMPLE_RATE_EXACT,
    0, AudioSynthWavetableV2::LoopMode::CONTINUOUS,
    0, 0, 26000, 0, 8, 0, 8, 600, 0);

static const AudioSynthWavetableV2::sample_data sawSample(
    sawWave, 33, 5, 4294967296.0f / AUDIO_SAMPLE_RATE_EXACT,
    0, AudioSynthWavetableV2::LoopMode::CONTINUOUS,
    0, 0, 18000, 0, 8, 0, 8, 600, 0);

static const AudioSynthWavetableV2::zone_data zones[] = {
    AudioSynthWavetableV2::zone_data(0, 127, 1, 127, sineSample),
    AudioSynthWavetableV2::zone_data(48, 84, 80, 127, sawSample)
};

static const AudioSynthWavetableV2::instrument_data instrument(zones, 2);

void setup() {
    Serial.begin(115200);
    AudioMemory(24);

    codec.enable();
    codec.volume(0.20f);

    if (!synth.setInstrument(instrument)) {
        Serial.println("Invalid V2 instrument data");
        while (true) delay(1000);
    }

    synth.amplitude(0.35f);
}

void loop() {
    const uint8_t notes[] = {60, 64, 67, 72};
    const int velocities[] = {105, 72, 110, 95};

    for (unsigned int i = 0; i < sizeof(notes); ++i) {
        const uint8_t layers = synth.noteOn(notes[i], velocities[i]);
        Serial.printf("Note %d, velocity %d: %d sample layer(s)\n",
                      notes[i], velocities[i], layers);
    }

    delay(500);

    for (unsigned int i = 0; i < sizeof(notes); ++i) {
        synth.noteOff(notes[i]);
    }

    delay(1200);
}
