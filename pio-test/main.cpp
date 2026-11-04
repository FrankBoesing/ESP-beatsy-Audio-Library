#include <Arduino.h>
#include <Audio.h>
#include <SD_MMC.h>

// ESP32-P4-WIFI6 test:
// - Records directly from AudioSynthWavetableV2 to SD_MMC as mono WAV.
// - No I2S output, codec or headphone adapter is required.
// - The three recorded sections exercise Sustain, normal ADSR and Release.
//
// Test loop:
//   LOOP_PHASE_END    = 2.0 PCM frames
//   LOOP_PHASE_LENGTH = 0.25 PCM frames
//   loopStart         = 1.75 PCM frames
// The old boundary threshold was 1.0 frame. It therefore replaced the
// adjacent PCM frame before playback had reached the 1.75-frame loop start.
//
// At MIDI note 48 and 44.1 kHz, the chosen phase increment is about 16
// phase units per output sample: frame 1 is reached after about 23.2 ms,
// loopStart after about 40.6 ms, and loopEnd after about 46.4 ms.

static const uint32_t RECORD_RATE = (uint32_t)AUDIO_SAMPLE_RATE_EXACT;
static const char *const RECORD_BASE_PATH = "/loop_boundary";
static const uint8_t TEST_VELOCITY = 127;

static const int16_t loopTestPcm[] = {
    0,
    12000,
    -12000,
    0
};

// Deliberately slow phase progression makes the pre-loop interval easy to
// inspect in Audacity. This is a diagnostic sample, not a musical tuning.
static const float TEST_PHASE_PER_HZ = 0.1223122f;

using SampleData = AudioSynthWavetableV2::sample_data;
using ZoneData = AudioSynthWavetableV2::zone_data;
using InstrumentData = AudioSynthWavetableV2::instrument_data;

static const SampleData sustainSample(
    loopTestPcm, 4, TEST_PHASE_PER_HZ, 0,
    AudioSynthWavetableV2::LoopMode::CONTINUOUS,
    0x8000U, 0x1000U, UINT16_MAX,
    0, 1, 0, 1, 160, 0);

static const SampleData adsrSample(
    loopTestPcm, 4, TEST_PHASE_PER_HZ, 0,
    AudioSynthWavetableV2::LoopMode::CONTINUOUS,
    0x8000U, 0x1000U, UINT16_MAX,
    0, 1200, 0, 1, 160, 0);

static const SampleData releaseSample(
    loopTestPcm, 4, TEST_PHASE_PER_HZ, 0,
    AudioSynthWavetableV2::LoopMode::CONTINUOUS,
    0x8000U, 0x1000U, UINT16_MAX,
    0, 1, 0, 1, 160, 0);

static const ZoneData testZones[] = {
    ZoneData(48, 48, 1, 127, sustainSample), // reaches Sustain quickly
    ZoneData(49, 49, 1, 127, adsrSample),    // long Attack: normal ADSR path
    ZoneData(50, 50, 1, 127, releaseSample) // Note-Off exercises Release path
};

static const InstrumentData testInstrument(
    testZones, sizeof(testZones) / sizeof(testZones[0]));

AudioSynthWavetableV2 synth;
AudioRecordFile recorder;
AudioConnection synthToRecorder(synth, 0, recorder, 0);

static void stopWithError(const char *message)
{
    Serial.println(message);
    while (true) {
        delay(1000);
    }
}

static bool mountSD()
{
#if defined(PIN_SD_MMC_CLK) && defined(PIN_SD_MMC_CMD) && \
    defined(PIN_SD_MMC_D0) && defined(PIN_SD_MMC_D1) && \
    defined(PIN_SD_MMC_D2) && defined(PIN_SD_MMC_D3)
    if (!SD_MMC.setPins(PIN_SD_MMC_CLK, PIN_SD_MMC_CMD,
                        PIN_SD_MMC_D0, PIN_SD_MMC_D1,
                        PIN_SD_MMC_D2, PIN_SD_MMC_D3)) {
        return false;
    }
#endif

    // 1-bit mode is sufficient for this short mono PCM recording.
    return SD_MMC.begin("/sdcard", true);
}

static void runTestCase(uint8_t note, const char *name,
                        uint32_t sustainOrAttackMs)
{
    Serial.printf("CASE_START,%s,note=%u,t_ms=%lu\n",
                  name, (unsigned)note, (unsigned long)millis());

    if (synth.noteOn(note, TEST_VELOCITY) != 1) {
        stopWithError("ERROR: noteOn did not start exactly one sample layer.");
    }

    delay(sustainOrAttackMs);

    if (!synth.noteOff(note)) {
        stopWithError("ERROR: noteOff failed.");
    }

    // releaseCount=160 means 160 * ENVELOPE_PERIOD (8) output samples.
    // Keep a little extra time so the complete release tail is recorded.
    delay(50);
    synth.allSoundOff();
    delay(30); // visible silence between the three sections

    Serial.printf("CASE_END,%s,t_ms=%lu\n",
                  name, (unsigned long)millis());
}

void setup()
{
    Serial.begin(115200);
    delay(500);

    Serial.println();
    Serial.println("Wavetable V2 loop-boundary WAV test");
    Serial.printf("Audio sample rate: %lu Hz\n",
                  (unsigned long)RECORD_RATE);

    if (!mountSD()) {
        stopWithError("ERROR: SD_MMC mount failed. Check the card and P4 pin mapping.");
    }

    // Preserve any earlier recording; remove/rename it manually before rerunning.
    if (SD_MMC.exists("/loop_boundary.wav") ||
        SD_MMC.exists("/loop_boundary.tmp")) {
        stopWithError("ERROR: /loop_boundary.wav or .tmp already exists. Rename or remove it first.");
    }

    AudioMemory(16);
    delay(100);

    if (!synth.setInstrument(testInstrument)) {
        stopWithError("ERROR: test instrument validation failed.");
    }
    synth.amplitude(0.8f);

    if (!recorder.begin(SD_MMC, RECORD_BASE_PATH, RECORD_RATE, 1,
                        AudioRecordFile::CORE_UNPINNED)) {
        stopWithError("ERROR: AudioRecordFile.begin failed. Check PSRAM and SD card.");
    }

    delay(40); // recorder and writer task pre-roll

    Serial.println("Recording /loop_boundary.wav");
    Serial.println("Sections: SUSTAIN (MIDI 48), ADSR (MIDI 49), RELEASE (MIDI 50)");

    runTestCase(48, "SUSTAIN", 130);
    runTestCase(49, "ADSR", 130);
    // Note-Off at 25 ms: phase is between frame 1.0 and loopStart 1.75.
    // The Release renderer must cross the same critical pre-loop interval.
    runTestCase(50, "RELEASE", 25);

    if (recorder.stop()) {
        Serial.println("SUCCESS: saved /loop_boundary.wav");
        Serial.println("Copy the WAV from the SD card and inspect it in Audacity.");
    } else {
        Serial.printf("ERROR: recording finalization failed; recorder error=%s. "
                      "The .tmp file may be recoverable.\n",
                      recorder.hasError() ? "yes" : "no");
    }
}

void loop()
{
    delay(1000);
}
