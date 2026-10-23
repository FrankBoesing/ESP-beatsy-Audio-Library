/*
 * Cathedral Bells - standalone VERBtrix demonstration.
 *
 * A procedural modal synthesizer generates
 * slow bell-like strikes, which are sent through AudioEffectVintageReverb
 * to stereo I2S output.
 *
 * Optional: record the post-reverb stereo signal as a WAV file on SD_MMC.
 * Set CATHEDRAL_RECORD_TO_SD to 0 to build without SD-card recording.
 *
 * Original reverb DSP core by Bas Scheffers, MIT license.
 */

#ifndef CATHEDRAL_RECORD_TO_SD
#define CATHEDRAL_RECORD_TO_SD 0
#endif

// The P4 SDMMC bus can be tested in 1-bit mode if 4-bit transfers report CRC
// or timeout errors. One data line has ample throughput for 44.1/48 kHz stereo
// PCM. Set to 0 to use all four data lines (D0-D3).
#ifndef CATHEDRAL_SDMMC_1BIT
#define CATHEDRAL_SDMMC_1BIT 1
#endif

// The WAV file is finalized automatically after this duration.
// Override from build flags if a longer/shorter sample is wanted.
#ifndef CATHEDRAL_RECORD_DURATION_MS
#define CATHEDRAL_RECORD_DURATION_MS 60000UL
#endif

#include <Audio.h>

#if CATHEDRAL_RECORD_TO_SD
#include <SD_MMC.h>
#endif

AudioOutputI2S audioOut({
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

AudioSynthHandpan bells;
AudioEffectVintageReverb cathedral;

#if CATHEDRAL_RECORD_TO_SD
AudioRecordFile recorder;
#endif

AudioConnection bellsToLeft(bells, 0, cathedral, 0);
AudioConnection bellsToRight(bells, 0, cathedral, 1);
AudioConnection reverbToLeft(cathedral, 0, audioOut, 0);
AudioConnection reverbToRight(cathedral, 1, audioOut, 1);

#if CATHEDRAL_RECORD_TO_SD
// Tap the stereo signal after reverb, in parallel with the I2S output.
AudioConnection reverbToRecordLeft(cathedral, 0, recorder, 0);
AudioConnection reverbToRecordRight(cathedral, 1, recorder, 1);
#endif

namespace {

constexpr uint8_t BELL_COUNT = 8;
constexpr uint8_t BELL_MODE_COUNT = 6;

constexpr float BELL_ROOT_HZ[BELL_COUNT] = {
    130.813f, 164.814f, 196.000f, 261.626f,
    293.665f, 329.628f, 392.000f, 523.251f
};

// Inharmonic partials give the strike a bell-like rather than organ-like tone.
// The reverb provides the long architectural tail.
constexpr float PARTIAL_RATIO[BELL_MODE_COUNT] = {
    1.0f, 2.0f, 2.4f, 3.0f, 4.1f, 5.4f
};
constexpr float PARTIAL_GAIN[BELL_MODE_COUNT] = {
    0.24f, 0.055f, 0.15f, 0.045f, 0.035f, 0.018f
};
constexpr float PARTIAL_T60[BELL_MODE_COUNT] = {
    5.5f, 3.5f, 4.5f, 3.0f, 2.5f, 1.8f
};

struct BellEvent {
    uint32_t atMs;
    uint8_t bell;
    uint8_t velocity;
};

// A slow, sparse phrase. The gaps let the cathedral tail remain audible.
constexpr BellEvent BELL_PHRASE[] = {
    {     0, 3, 92 }, // C4
    {   950, 6, 78 }, // G4
    {  2100, 7, 86 }, // C5
    {  3650, 4, 72 }, // D4
    {  5200, 5, 80 }, // E4
    {  7100, 6, 82 }, // G4
    {  8950, 2, 76 }, // G3
    { 10900, 5, 78 }, // E4
    { 12700, 7, 86 }, // C5
    { 14600, 4, 72 }, // D4
    { 16400, 3, 88 }, // C4
};

constexpr size_t BELL_EVENT_COUNT =
    sizeof(BELL_PHRASE) / sizeof(BELL_PHRASE[0]);
constexpr uint32_t PHRASE_LENGTH_MS = 26000;

uint32_t phraseStartMs = 0;
size_t nextBellEvent = 0;
uint32_t droppedBellEvents = 0;

#if CATHEDRAL_RECORD_TO_SD
constexpr uint32_t RECORD_SAMPLE_RATE = (uint32_t)AUDIO_SAMPLE_RATE_EXACT;
bool recordingActive = false;
uint32_t recordingStartMs = 0;

bool mountSD()
{
#if defined(PIN_SD_MMC_CLK) && defined(PIN_SD_MMC_CMD) && \
    defined(PIN_SD_MMC_D0) && defined(PIN_SD_MMC_D1) && \
    defined(PIN_SD_MMC_D2) && defined(PIN_SD_MMC_D3)
    if (!SD_MMC.setPins(PIN_SD_MMC_CLK, PIN_SD_MMC_CMD,
                        PIN_SD_MMC_D0, PIN_SD_MMC_D1,
                        PIN_SD_MMC_D2, PIN_SD_MMC_D3)) {
        Serial.println("ERROR: SD_MMC pin configuration failed");
        return false;
    }
#endif

    Serial.printf("Initializing SD_MMC in %u-bit mode\\n",
                  CATHEDRAL_SDMMC_1BIT ? 1U : 4U);

    // 1-bit mode is sufficient for PCM recording and is a useful diagnostic
    // when 4-bit data-line integrity is uncertain.
    if (!SD_MMC.begin("/sdcard", CATHEDRAL_SDMMC_1BIT != 0)) {
        Serial.println("WARNING: SD_MMC mount failed; continuing without recording");
        return false;
    }

    return true;
}

void finishRecording(const char *reason)
{
    if (!recordingActive) {
        return;
    }

    recordingActive = false;
    Serial.println(reason);

    if (recorder.stop()) {
        Serial.println("Saved /cathedral_bells.wav to SD card");
    } else {
        Serial.printf("WAV finalization failed (recorder error=%s); "
                      "/cathedral_bells.tmp may remain\n",
                      recorder.hasError() ? "yes" : "no");
    }
}
#endif

bool configureBells()
{
    for (uint8_t bell = 0; bell < BELL_COUNT; ++bell) {
        AudioSynthHandpan::ModeConfig modes[BELL_MODE_COUNT] = {};

        for (uint8_t mode = 0; mode < BELL_MODE_COUNT; ++mode) {
            modes[mode].frequencyHz = BELL_ROOT_HZ[bell] * PARTIAL_RATIO[mode];
            modes[mode].gain = PARTIAL_GAIN[mode];
            modes[mode].decaySeconds = PARTIAL_T60[mode];
            modes[mode].phaseRadians = 0.0f;
        }

        if (!bells.configureNote(bell, modes, BELL_MODE_COUNT)) {
            Serial.printf("Could not configure bell %u\n", (unsigned)bell);
            return false;
        }
    }

    return true;
}

bool timeReached(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

void playBellEvents(uint32_t now)
{
    while (timeReached(now, phraseStartMs + PHRASE_LENGTH_MS)) {
        phraseStartMs += PHRASE_LENGTH_MS;
        nextBellEvent = 0;

        if (droppedBellEvents != 0) {
            Serial.printf("Warning: %lu bell strikes were dropped\n",
                          (unsigned long)droppedBellEvents);
            droppedBellEvents = 0;
        }
    }

    while (nextBellEvent < BELL_EVENT_COUNT) {
        const BellEvent &event = BELL_PHRASE[nextBellEvent];
        const uint32_t dueAt = phraseStartMs + event.atMs;

        if (!timeReached(now, dueAt)) {
            break;
        }

        if (!bells.noteOn(event.bell, event.velocity)) {
            ++droppedBellEvents;
        }

        ++nextBellEvent;
    }
}

} // namespace

void setup()
{
    Serial.begin(115200);
    delay(300);

    Serial.println();
    Serial.println("VERBtrix Cathedral Bells");
    Serial.println("Synthetic bell strikes -> vintage reverb -> stereo I2S");

#if CATHEDRAL_RECORD_TO_SD
    const bool sdMounted = mountSD();
    if (!sdMounted) {
        Serial.println("SD recording disabled for this run; audio playback continues");
    }
#else
    Serial.println("SD recording disabled (CATHEDRAL_RECORD_TO_SD=0)");
#endif

    // The modal synth must be configured before the audio graph starts.
    if (!configureBells()) {
        Serial.println("ERROR: bell configuration failed");
        while (true) {
            delay(1000);
        }
    }

    // Use the long-decay cathedral preset as the starting point.
    if (!cathedral.begin()) {
        Serial.println("WARNING: reverb initialization failed; playing dry bells");
    } else {
        cathedral.preset(AudioEffectVintageReverb::LARGE_CATHEDRAL);
        cathedral.mix(0.82f);
        cathedral.wetGain(0.85f);
        cathedral.modDepth(0.35f);
    }

    AudioMemory(48);

    codec.enable();
    codec.volume(0.20f);

    Serial.printf("Reverb storage: %s; ready=%s\n",
                  cathedral.usingPSRAM() ? "PSRAM" : "internal RAM",
                  cathedral.ready() ? "yes" : "no");

#if CATHEDRAL_RECORD_TO_SD
    if (sdMounted) {
        if (SD_MMC.exists("/cathedral_bells.wav") ||
            SD_MMC.exists("/cathedral_bells.tmp")) {
            Serial.println("Recording skipped: /cathedral_bells.wav or .tmp already exists");
            Serial.println("Rename/remove the old file before recording again");
        } else if (recorder.begin(SD_MMC, "/cathedral_bells",
                                  RECORD_SAMPLE_RATE, 2,
                                  AudioRecordFile::CORE_UNPINNED)) {
            recordingActive = true;
            recordingStartMs = millis();
            Serial.printf("Recording stereo WAV at %lu Hz for %lu seconds\n",
                          (unsigned long)RECORD_SAMPLE_RATE,
                          (unsigned long)(CATHEDRAL_RECORD_DURATION_MS / 1000UL));
        } else {
            Serial.println("WARNING: could not start SD recording; check SD card and PSRAM");
        }
    }
#endif

    phraseStartMs = millis();
    nextBellEvent = 0;
}

void loop()
{
    const uint32_t now = millis();
    playBellEvents(now);

#if CATHEDRAL_RECORD_TO_SD
    if (recordingActive) {
        if (!recorder.isRecording()) {
            finishRecording("Recording stopped unexpectedly; finalizing partial file");
        } else if ((uint32_t)(now - recordingStartMs) >= CATHEDRAL_RECORD_DURATION_MS) {
            finishRecording("Recording duration reached; finalizing WAV file");
        }
    }
#endif

    static uint32_t lastStatsMs = 0;
    if (now - lastStatsMs >= 5000) {
        lastStatsMs = now;
        Serial.printf("Audio CPU: %.1f%% (max %.1f%%), blocks: %u/%u\n",
                      AudioProcessorUsage(), AudioProcessorUsageMax(),
                      AudioMemoryUsage(), AudioMemoryUsageMax());
        AudioProcessorUsageMaxReset();
        AudioMemoryUsageMaxReset();
    }
}
