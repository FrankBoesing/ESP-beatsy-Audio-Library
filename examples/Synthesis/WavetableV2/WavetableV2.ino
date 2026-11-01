/*
 * AudioSynthWavetableV2 stress test and WAV recorder.
 *
 * Starts 32 notes with four matching sample layers each (128 sample voices),
 * records the mono synthesizer output to /wavetable_v2.wav on SD_MMC and
 * reports both total audio-graph load and the V2 node load.
 *
 * SD_MMC pin definitions may be supplied by platformio.ini.
 */

#include <Arduino.h>
#include <FS.h>
#include <SD_MMC.h>
#include <Audio.h>

constexpr uint32_t RECORD_SAMPLE_RATE = AUDIO_SAMPLE_RATE_EXACT;
constexpr uint8_t TEST_NOTE_COUNT = AudioSynthWavetableV2::MAX_NOTES;
constexpr int TEST_FIRST_NOTE = 48;
constexpr int TEST_VELOCITY = 112;

AudioSynthWavetableV2 synth;
AudioRecordFile recorder;
AudioConnection synthToRecorder(synth, recorder);

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

// All four zones overlap. Each tested note therefore starts four sample layers.
static const AudioSynthWavetableV2::zone_data zones[] = {
    AudioSynthWavetableV2::zone_data(48, 84, 96, 127, sineSample),
    AudioSynthWavetableV2::zone_data(48, 84, 96, 127, sawSample),
    AudioSynthWavetableV2::zone_data(48, 84, 96, 127, triangleSample),
    AudioSynthWavetableV2::zone_data(48, 84, 96, 127, squareSample)
};

static const AudioSynthWavetableV2::instrument_data instrument(zones, 4);

static void haltWithError(const char *message) {
    Serial.println(message);
    while (true) {
        delay(1000);
    }
}

static bool mountSD() {
#if defined(PIN_SD_MMC_CLK) && defined(PIN_SD_MMC_CMD) && \
    defined(PIN_SD_MMC_D0) && defined(PIN_SD_MMC_D1) && \
    defined(PIN_SD_MMC_D2) && defined(PIN_SD_MMC_D3)
    if (!SD_MMC.setPins(PIN_SD_MMC_CLK, PIN_SD_MMC_CMD,
                        PIN_SD_MMC_D0, PIN_SD_MMC_D1,
                        PIN_SD_MMC_D2, PIN_SD_MMC_D3)) {
        return false;
    }
#endif
    return SD_MMC.begin("/sdcard", false);
}

static void printLoad() {
    Serial.printf(
        "Audio CPU: graph %5.1f%% (max %5.1f%%), V2 %5.1f%% (max %5.1f%%), "
        "notes %u, sample voices %u, audio blocks %u/%u, recording %s\n",
        AudioProcessorUsageTotal(), AudioProcessorUsageTotalMax(),
        synth.AudioProcessorUsage(), synth.AudioProcessorUsageMax(),
        (unsigned)synth.activeNoteCount(), (unsigned)synth.activeSampleCount(),
        (unsigned)AudioMemoryUsage(), (unsigned)AudioMemoryUsageMax(),
        recorder.isRecording() ? "yes" : "no");
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

void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println();
    Serial.println("AudioSynthWavetableV2: 32 notes / up to 128 sample voices");
    Serial.println("The synthesizer output is recorded as mono 16-bit PCM WAV.");

    if (!mountSD()) {
        haltWithError("SD_MMC mount failed. Check card, DIP switches and pin configuration.");
    }

    if (!psramFound()) {
        haltWithError("PSRAM is required by AudioRecordFile.");
    }

    AudioMemory(24);

    if (!synth.setInstrument(instrument)) {
        haltWithError("Invalid V2 instrument data.");
    }
    synth.amplitude(0.20f);

    // This example overwrites only its own previous completed output file.
    // An existing .tmp file is deliberately preserved for possible recovery.
    if (SD_MMC.exists("/wavetable_v2.wav")) {
        SD_MMC.remove("/wavetable_v2.wav");
    }

    if (!recorder.begin(SD_MMC, "/wavetable_v2", RECORD_SAMPLE_RATE, 1,
                        AudioRecordFile::CORE_UNPINNED)) {
        haltWithError("Could not start WAV recorder. Check PSRAM and /wavetable_v2.tmp.");
    }

    delay(100);
    AudioProcessorUsageTotalMaxReset();
    synth.AudioProcessorUsageMaxReset();
    AudioMemoryUsageMaxReset();

    uint8_t startedNotes = 0;
    uint16_t startedSamples = 0;

    for (uint8_t i = 0; i < TEST_NOTE_COUNT; ++i) {
        const int note = TEST_FIRST_NOTE + i;
        const uint8_t layers = synth.noteOn(note, TEST_VELOCITY);
        if (layers != 0) {
            ++startedNotes;
            startedSamples += layers;
        }
    }

    Serial.printf("Started %u notes and %u sample layers. Expected maximum: 32 / 128.\n",
                  (unsigned)startedNotes, (unsigned)startedSamples);

    // Keep the 32-note / 128-layer load active while recording.
    monitorFor(3000);

    synth.allNotesOff();
    Serial.println("Note-Off sent; recording the release tails.");
    monitorFor(1800);

    if (recorder.stop()) {
        Serial.println("Saved /wavetable_v2.wav to SD card.");
    } else {
        Serial.printf("WAV finalization failed; recorder error=%s. "
                      "Check /wavetable_v2.tmp for recovery.\n",
                      recorder.hasError() ? "yes" : "no");
    }

    printLoad();
}

void loop() {
    delay(1000);
}
