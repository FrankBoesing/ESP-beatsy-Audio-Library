/*
 * AudioSynthWavetableV2 stress test -> USB or I2S output.
 *
 * Sweeps note counts 1, 2, 4, 8, 16 and 32 (up to the configured maximum),
 * with four matching sample layers per note. This measures the V2 renderer
 * across 4..128 active sample voices without repeated rebuilds.
 *
 * Define WAVETABLE_V2_TEST_STEREO=1 to replace the fourth mono layer with a
 * linked left/right sample and exercise mixed mono/stereo rendering. Build once
 * with stereo disabled and once enabled to compare mono and mixed-stereo cost.
 *
 * WAVETABLE_V2_TEST_NOTE_COUNT sets the maximum stage. Set it to 32 for the
 * complete sweep; smaller values produce shorter sweeps. Run the same build
 * and sweep on the ESP32-S3 and ESP32-P4 at 44.1 and 48 kHz.
 * Record USB capture with Audacity, or capture the I2S codec output externally.
 *
 * PlatformIO:
 *   ESP32-S3 USB: env:freenove_esp32_s3_display_USBAUDIO (48 kHz UAC1)
 *   ESP32-S3 I2S: env:freenove_esp32_s3_display (44.1/48 kHz, ES8311)
 *   ESP32-P4 I2S: env:esp32-p4 (44.1/48 kHz, ES8311)
 *
 * For I2S builds, define WAVETABLE_V2_TEST_RATE=44100 or 48000 to select the
 * output rate before AudioMemory() starts the audio clock. USB UAC1 uses the
 * host/device rate configured by its descriptor; the current test environment
 * is fixed at 48 kHz.
 */

#if defined(AUDIO_OUTPUT_USB)
#if !defined(ARDUINO_USB_MODE) || ARDUINO_USB_MODE != 0 || \
    !defined(ARDUINO_USB_CDC_ON_BOOT) || ARDUINO_USB_CDC_ON_BOOT != 0
#error "WavetableV2 USB output requires TinyUSB mode and ARDUINO_USB_CDC_ON_BOOT=0."
#endif
#else
#if !defined(PIN_I2S_BLCK) || !defined(PIN_I2S_WS) || !defined(PIN_I2S_DOUT)
#error "Define PIN_I2S_BLCK, PIN_I2S_WS and PIN_I2S_DOUT for I2S benchmark output."
#endif
#ifndef PIN_I2S_MLCK
#define PIN_I2S_MLCK -1
#endif
#endif

#include <Arduino.h>
#include <Audio.h>

// Maximum note count for the automatic 1/2/4/8/16/32-note sweep.
// For the full range use -D WAVETABLE_V2_TEST_NOTE_COUNT=32.
#ifndef WAVETABLE_V2_TEST_NOTE_COUNT
#define WAVETABLE_V2_TEST_NOTE_COUNT 1
#endif

#ifndef WAVETABLE_V2_TEST_STEREO
#define WAVETABLE_V2_TEST_STEREO 0
#endif

constexpr uint8_t TEST_NOTE_COUNT = WAVETABLE_V2_TEST_NOTE_COUNT;
static_assert(TEST_NOTE_COUNT >= 1 &&
              TEST_NOTE_COUNT <= AudioSynthWavetableV2::MAX_NOTES,
              "WAVETABLE_V2_TEST_NOTE_COUNT must be between 1 and MAX_NOTES");
constexpr int TEST_FIRST_NOTE = 48;
constexpr int TEST_VELOCITY = 112;

AudioSynthWavetableV2 synth;
#if defined(AUDIO_OUTPUT_USB)
AudioOutputUSB audioOutput;
#else
AudioOutputI2S audioOutput({
    PIN_I2S_BLCK,
    PIN_I2S_WS,
    PIN_I2S_DOUT,
    PIN_I2S_MLCK
});
#if defined(AUDIO_CODEC_ES8311)
AudioControlES8311 codec({PIN_I2C_SDA, PIN_I2C_SCL, PIN_AMPLIFIER, PIN_AMPLIFIER_ACTIVE});
#elif defined(AUDIO_CODEC_ES8388)
AudioControlES8388 codec({PIN_I2C_SDA, PIN_I2C_SCL, PIN_AMPLIFIER, PIN_AMPLIFIER_ACTIVE});
#endif
#endif
AudioConnection synthToOutputLeft(synth, 0, audioOutput, 0);
AudioConnection synthToOutputRight(synth, 1, audioOutput, 1);

#if defined(AUDIO_OUTPUT_USB)
#if defined(WAVETABLE_V2_TEST_RATE)
static_assert(WAVETABLE_V2_TEST_RATE == (uint32_t)AUDIO_SAMPLE_RATE_EXACT,
              "USB UAC1 rate is fixed by the descriptor; WAVETABLE_V2_TEST_RATE must match it.");
#endif
constexpr uint32_t TEST_OUTPUT_RATE = (uint32_t)AUDIO_SAMPLE_RATE_EXACT;
#elif defined(WAVETABLE_V2_TEST_RATE)
static_assert(WAVETABLE_V2_TEST_RATE == 44100 || WAVETABLE_V2_TEST_RATE == 48000,
              "WAVETABLE_V2_TEST_RATE must be 44100 or 48000.");
constexpr uint32_t TEST_OUTPUT_RATE = WAVETABLE_V2_TEST_RATE;
#else
constexpr uint32_t TEST_OUTPUT_RATE = (uint32_t)AUDIO_SAMPLE_RATE_EXACT;
#endif

// 32 playable frames plus one interpolation guard frame.
// Each table is one single-cycle waveform; phase wraps naturally at 32 frames.
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

static const int16_t triangleWave[33] = {
    -32768, -28672, -24576, -20480, -16384, -12288, -8192, -4096,
    0, 4096, 8192, 12288, 16384, 20480, 24576, 28672,
    32767, 28672, 24576, 20480, 16384, 12288, 8192, 4096,
    0, -4096, -8192, -12288, -16384, -20480, -24576, -28672,
    -32768
};

static const int16_t squareWave[33] = {
    -32768, -32768, -32768, -32768, -32768, -32768, -32768, -32768,
    -32768, -32768, -32768, -32768, -32768, -32768, -32768, -32768,
     32767,  32767,  32767,  32767,  32767,  32767,  32767,  32767,
     32767,  32767,  32767,  32767,  32767,  32767,  32767,  32767,
    -32768
};

static const AudioSynthWavetableV2::sample_data sineSample(
    sineWave, 33, 5, 4294967296.0f / AUDIO_SAMPLE_RATE_EXACT,
    0, AudioSynthWavetableV2::LoopMode::CONTINUOUS,
    0, 0, 15000, 0, 8, 0, 8, 700, 0);

static const AudioSynthWavetableV2::sample_data sawSample(
    sawWave, 33, 5, 4294967296.0f / AUDIO_SAMPLE_RATE_EXACT,
    0, AudioSynthWavetableV2::LoopMode::CONTINUOUS,
    0, 0, 9000, 0, 8, 0, 8, 700, 0);

static const AudioSynthWavetableV2::sample_data triangleSample(
    triangleWave, 33, 5, 4294967296.0f / AUDIO_SAMPLE_RATE_EXACT,
    0, AudioSynthWavetableV2::LoopMode::CONTINUOUS,
    0, 0, 9000, 0, 8, 0, 8, 700, 0);

static const AudioSynthWavetableV2::sample_data squareSample(
    squareWave, 33, 5, 4294967296.0f / AUDIO_SAMPLE_RATE_EXACT,
    0, AudioSynthWavetableV2::LoopMode::CONTINUOUS,
    0, 0, 5000, 0, 8, 0, 8, 700, 0);

#if WAVETABLE_V2_TEST_STEREO
// Deliberately different waveforms make the left/right output distinguishable.
static const AudioSynthWavetableV2::sample_data stereoTestSample(
    sineWave, 33, 5, 4294967296.0f / AUDIO_SAMPLE_RATE_EXACT,
    0, AudioSynthWavetableV2::LoopMode::CONTINUOUS,
    0, 0, 5000, 0, 8, 0, 8, 700, 0, sawWave);
#endif

// All four zones overlap. Each tested note therefore starts four sample layers.
static const AudioSynthWavetableV2::zone_data zones[] = {
    AudioSynthWavetableV2::zone_data(48, 84, 96, 127, sineSample),
    AudioSynthWavetableV2::zone_data(48, 84, 96, 127, sawSample),
    AudioSynthWavetableV2::zone_data(48, 84, 96, 127, triangleSample),
#if WAVETABLE_V2_TEST_STEREO
    AudioSynthWavetableV2::zone_data(48, 84, 96, 127, stereoTestSample)
#else
    AudioSynthWavetableV2::zone_data(48, 84, 96, 127, squareSample)
#endif
};

static const AudioSynthWavetableV2::instrument_data instrument(zones, 4);

static void consolePrint(const char *text) {
    Serial.print(text);
#if defined(AUDIO_OUTPUT_USB)
    USBCDC &usbCdc = AudioUSBSerial();
    if (usbCdc) usbCdc.print(text);
#endif
}

static void consolePrint(uint32_t value) {
    Serial.print(value);
#if defined(AUDIO_OUTPUT_USB)
    USBCDC &usbCdc = AudioUSBSerial();
    if (usbCdc) usbCdc.print(value);
#endif
}

static void consolePrint(float value, uint8_t digits = 1) {
    Serial.print(value, digits);
#if defined(AUDIO_OUTPUT_USB)
    USBCDC &usbCdc = AudioUSBSerial();
    if (usbCdc) usbCdc.print(value, digits);
#endif
}

static void consolePrintln(const char *text) {
    Serial.println(text);
#if defined(AUDIO_OUTPUT_USB)
    USBCDC &usbCdc = AudioUSBSerial();
    if (usbCdc) usbCdc.println(text);
#endif
}

static void consolePrintln(uint32_t value) {
    Serial.println(value);
#if defined(AUDIO_OUTPUT_USB)
    USBCDC &usbCdc = AudioUSBSerial();
    if (usbCdc) usbCdc.println(value);
#endif
}

static void haltWithError(const char *message) {
    consolePrintln(message);
    while (true) {
        delay(1000);
    }
}

static void printLoad() {
    consolePrint("Audio CPU: graph ");
    consolePrint(AudioStream::AudioProcessorUsageTotal(), 1);
    consolePrint("% (max ");
    consolePrint(AudioStream::AudioProcessorUsageTotalMax(), 1);
    consolePrint("%), V2 ");
    consolePrint(synth.AudioProcessorUsage(), 1);
    consolePrint("% (max ");
    consolePrint(synth.AudioProcessorUsageMax(), 1);
    consolePrint("%), notes ");
    consolePrint((uint32_t)synth.activeNoteCount());
    consolePrint(", sample voices ");
    consolePrint((uint32_t)synth.activeSampleCount());
    consolePrint(", audio blocks ");
    consolePrint((uint32_t)AudioMemoryUsage());
    consolePrint("/");
    consolePrint((uint32_t)AudioMemoryUsageMax());
#if defined(AUDIO_OUTPUT_USB)
    consolePrint(", USB configured ");
    consolePrintln(audioOutput.isConfigured() ? "yes" : "no");
#else
    consolePrint(", I2S running ");
    consolePrintln(audioOutput.isRunning() ? "yes" : "no");
#endif
}

static void printPhasePeak(const char *phase) {
    consolePrint(phase);
    consolePrint(" peak: graph ");
    consolePrint(AudioStream::AudioProcessorUsageTotalMax(), 1);
    consolePrint("%, V2 ");
    consolePrint(synth.AudioProcessorUsageMax(), 1);
    consolePrintln("%");
}

static void monitorFor(uint32_t durationMs) {
    const uint32_t start = millis();
    uint32_t lastReport = start;

    while ((uint32_t)(millis() - start) < durationMs) {
        const uint32_t now = millis();
        if ((uint32_t)(now - lastReport) >= 500U) {
            lastReport = now;
            printLoad();
        }
        delay(20);
    }
}

static void playStressChord(uint8_t noteCount) {
    uint8_t startedNotes = 0;
    uint16_t startedSamples = 0;

    for (uint8_t i = 0; i < noteCount; ++i) {
        const int note = TEST_FIRST_NOTE + i;

        consolePrint("Calling noteOn for MIDI note ");
        consolePrint((uint32_t)note);
        consolePrintln(".");

        const uint8_t layers = synth.noteOn(note, TEST_VELOCITY);

        consolePrint("noteOn returned; layers = ");
        consolePrint((uint32_t)layers);
        consolePrintln(".");

        if (layers != 0) {
            ++startedNotes;
            startedSamples += layers;
        }
    }

    consolePrint("Started notes: ");
    consolePrint((uint32_t)startedNotes);
    consolePrint(", sample layers: ");
    consolePrint((uint32_t)startedSamples);
    consolePrintln(" started.");
}

static void printBenchmarkRow(const char *phase, uint8_t requestedNotes) {
    consolePrint("BENCH,");
    consolePrint(phase);
    consolePrint(",");
    consolePrint(TEST_OUTPUT_RATE);
    consolePrint(",");
    consolePrint((uint32_t)requestedNotes);
    consolePrint(",");
    consolePrint((uint32_t)synth.activeNoteCount());
    consolePrint(",");
    consolePrint((uint32_t)synth.activeSampleCount());
    consolePrint(",");
    consolePrint(synth.cpu_time_us);
    consolePrint(",");
    consolePrint(synth.cpu_time_max_us);
    consolePrint(",");
    consolePrint(AudioStream::cpu_time_total_us);
    consolePrint(",");
    consolePrint(AudioStream::cpu_time_total_max_us);
    consolePrint(",");
    consolePrint((uint32_t)AudioMemoryUsage());
    consolePrint(",");
    consolePrint((uint32_t)AudioMemoryUsageMax());
    consolePrintln("");
}

static void runBenchmarkStage(uint8_t noteCount) {
    synth.allSoundOff();
    delay(100);

    AudioStream::AudioProcessorUsageTotalMaxReset();
    synth.AudioProcessorUsageMaxReset();
    AudioMemoryUsageMaxReset();

    consolePrintln("");
    consolePrint("===== BENCHMARK: ");
    consolePrint((uint32_t)noteCount);
    consolePrintln(" notes =====");
    playStressChord(noteCount);

    // Let note-on setup settle before the measured sustain interval.
    delay(200);
    AudioStream::AudioProcessorUsageTotalMaxReset();
    synth.AudioProcessorUsageMaxReset();
    AudioMemoryUsageMaxReset();
    monitorFor(3000);
    printBenchmarkRow("sustain", noteCount);
    printPhasePeak("Sustain");

    // Measure the release envelope separately from the sustained workload.
    AudioStream::AudioProcessorUsageTotalMaxReset();
    synth.AudioProcessorUsageMaxReset();
    synth.allNotesOff();
    consolePrintln("Note-Off sent; measuring release tails.");
    monitorFor(1200);
    printBenchmarkRow("release", noteCount);
    printPhasePeak("Release");

    // Hard-stop residual voices before the next stage, then leave a short gap.
    synth.allSoundOff();
    delay(150);
}

void setup() {
    // Keep UART0 available for the PlatformIO monitor. USB builds also mirror
    // diagnostics to the AudioManager-owned CDC interface after startup.
    Serial.begin(115200);

#if !defined(AUDIO_OUTPUT_USB) && defined(WAVETABLE_V2_TEST_RATE)
    // Select the hardware rate before AudioMemory() starts the I2S clock.
    if (!audioOutput.setSampleRate((float)TEST_OUTPUT_RATE)) {
        haltWithError("Could not configure the requested I2S sample rate.");
    }
#endif

    // USB composite audio or I2S hardware is started by the Audio graph.
    AudioMemory(24);
    delay(300);

    consolePrintln("");
    consolePrintln("AudioSynthWavetableV2 stress test.");
    consolePrint("Configured notes: ");
    consolePrint((uint32_t)TEST_NOTE_COUNT);
    consolePrint(", maximum sample voices: ");
    consolePrintln((uint32_t)(TEST_NOTE_COUNT * AudioSynthWavetableV2::MAX_SAMPLE_LAYERS_PER_NOTE));
#if defined(AUDIO_OUTPUT_USB)
#if WAVETABLE_V2_TEST_STEREO
    consolePrintln("USB output: mixed mono/stereo sample test, 16-bit PCM.");
#else
    consolePrintln("USB output: mono mix mirrored to stereo, 16-bit PCM.");
#endif
#else
#if WAVETABLE_V2_TEST_STEREO
    consolePrintln("I2S output: mixed mono/stereo sample test.");
#else
    consolePrintln("I2S output: mono mix mirrored to stereo.");
#endif
#endif
#if defined(AUDIO_OUTPUT_USB)
    consolePrintln("Select the ESP32 USB microphone in Audacity and start recording.");
#else
    consolePrintln("Record the codec output through line-in or an external recorder.");
#endif
    consolePrintln("BENCH CSV: phase,rate_hz,requested_notes,active_notes,active_layers,node_us,node_peak_us,graph_us,graph_peak_us,blocks,blocks_peak");

#if defined(AUDIO_OUTPUT_USB)
    if (!audioOutput.isConfigured()) {
        haltWithError("Managed AudioOutputUSB initialization failed. Check the USB build environment.");
    }
#else
    if (!audioOutput.isRunning() && !audioOutput.begin()) {
        haltWithError("AudioOutputI2S initialization failed. Check the I2S pin mapping.");
    }
#if defined(AUDIO_CODEC_ES8311)
    if (!codec.enable(TEST_OUTPUT_RATE) || !codec.volume(0.2f)) {
        haltWithError("ES8311 initialization failed.");
    }
#elif defined(AUDIO_CODEC_ES8388)
    if (!codec.enable() || !codec.volume(0.2f)) {
        haltWithError("ES8388 initialization failed.");
    }
#endif
#endif

    if (!synth.setInstrument(instrument)) {
        haltWithError("Invalid V2 instrument data.");
    }
    synth.amplitude(0.35f);

    consolePrint("Audio output rate: ");
    consolePrint(TEST_OUTPUT_RATE);
#if defined(AUDIO_OUTPUT_USB)
    consolePrintln(" Hz (USB UAC1). Waiting 5 seconds for Audacity...");
#else
    consolePrintln(" Hz (I2S). Waiting 5 seconds for external recording...");
#endif
    delay(5000);

    consolePrintln("Stress test started. It repeats until the board is reset.");
}

void loop() {
    // Run a logarithmic polyphony sweep in one firmware build.
    // The last stage is always the configured maximum, even when it is not
    // an exact power of two (for example, 6 produces 1, 2, 4, 6).
    uint8_t noteCount = 1;
    for (;;) {
        runBenchmarkStage(noteCount);
        if (noteCount >= TEST_NOTE_COUNT) break;

        uint16_t nextCount = (uint16_t)noteCount * 2U;
        if (nextCount > TEST_NOTE_COUNT) nextCount = TEST_NOTE_COUNT;
        noteCount = (uint8_t)nextCount;
    }

    consolePrintln("Sweep complete. Repeating in 1 second.");
    delay(1000);
}
