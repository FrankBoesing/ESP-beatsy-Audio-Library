/*
 * Elisa's Hand Pan — WAV recording of Für Elise, opening excerpt (Beethoven, WoO 59)
 *
 * Uses the first 30 seconds of the upper-staff melody from the current,
 * public-domain Mutopia Project MIDI derived from the Breitkopf & Härtel
 * (1888) edition. At 25.833 s, the isolated E6 is lowered to E5 for this
 * handpan arrangement; the original note timings are retained.
 * Source: https://www.mutopiaproject.org/cgibin/piece-info.cgi?id=931
 */

#include <Arduino.h>
#include <FS.h>
#include <SD_MMC.h>
#include <Audio.h>

// Record one complete performance as stereo 16-bit PCM WAV on SD_MMC.
// AudioRecordFile requires PSRAM for its ring buffer.
constexpr uint32_t OUTPUT_SAMPLE_RATE = AUDIO_SAMPLE_RATE_EXACT;
constexpr uint32_t PHRASE_DURATION_MS = 30000;
constexpr uint32_t POST_PHRASE_TAIL_MS = 8000;
constexpr uint32_t RECORD_DURATION_MS = PHRASE_DURATION_MS + POST_PHRASE_TAIL_MS;
constexpr char RECORD_BASE_PATH[] = "/ElisasHandPan";

// Standalone recording build; the musical parameters are unchanged.

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

AudioSynthHandpan handpan;
AudioEffectFreeverb hall;
AudioMixer4 dryWetMixer;
AudioRecordFile recorder;
AudioConnection handpanToHall(handpan, hall);
AudioConnection handpanToDryMixer(handpan, 0, dryWetMixer, 0);
AudioConnection hallToWetMixer(hall, 0, dryWetMixer, 1);
AudioConnection mixToLeft(dryWetMixer, 0, audioOut, 0);
AudioConnection mixToRight(dryWetMixer, 0, audioOut, 1);
// Record the post-mixer signal; duplicate mono to both channels for stereo WAV.
AudioConnection mixToRecordLeft(dryWetMixer, 0, recorder, 0);
AudioConnection mixToRecordRight(dryWetMixer, 0, recorder, 1);


// Pitches used in the first 30 seconds of the score-derived melody.
// The isolated E6 at 25.833 s is lowered by one octave to E5 for this arrangement.
// MIDI pitches after this adjustment: 60, 64, 65, 67, 68, 69, 71, 72, 74, 75, 76, 77.
constexpr uint8_t PAD_COUNT = 12;
// Pad-index to MIDI pitch: 60, 64, 65, 67, 68, 69, 71, 72, 74, 75, 76, 77.
constexpr float PAD_FREQUENCIES[PAD_COUNT] = {
    261.626f, 329.628f, 349.228f, 391.995f,
    415.305f, 440.000f, 493.883f, 523.251f,
    587.330f, 622.254f, 659.255f, 698.456f
};

// Three damped body modes keep the warm, handpan-like tone.
constexpr float BODY_T60[3] = {3.00f, 3.00f, 4.00f};
constexpr float BODY_RATIOS[3] = {1.000f, 2.011f, 2.984f};
constexpr float BODY_GAIN_BASE[3] = {0.30f, 0.31f, 0.52f};

// Three-body-mode levels vary by pad to avoid identical synthetic timbre.
// Keep resonator phase at zero in this listening test: each new strike then
// starts at a zero crossing instead of injecting an abrupt nonzero first sample.
constexpr float BODY_LEVELS[PAD_COUNT][3] = {
    {0.70f,0.10f,0.30f},
    {0.66f,0.18f,0.26f},
    {0.62f,0.27f,0.22f},
    {0.65f,0.42f,0.16f},
    {0.58f,0.35f,0.28f},
    {0.70f,0.55f,0.20f},
    {0.54f,0.44f,0.31f},
    {0.50f,0.75f,0.30f},
    {0.58f,0.68f,0.38f},
    {0.62f,0.24f,0.18f},
    {0.64f,0.31f,0.24f},
    {0.65f,0.23f,0.17f}
};

bool configureHandpan()
{
    for (uint8_t pad = 0; pad < PAD_COUNT; ++pad) {
        AudioSynthHandpan::ModeConfig modes[3] = {};
        const float f0 = PAD_FREQUENCIES[pad];

        for (uint8_t mode = 0; mode < 3; ++mode) {
            modes[mode].frequencyHz = f0 * BODY_RATIOS[mode];
            modes[mode].gain = BODY_GAIN_BASE[mode] * BODY_LEVELS[pad][mode];
            modes[mode].decaySeconds = BODY_T60[mode];
            modes[mode].phaseRadians = 0.0f;
        }

        // The synth supplies its built-in soft finger-contact transient.
        if (!handpan.configureNote(pad, modes, 3)) {
            Serial.print("Failed to configure pad ");
            Serial.println(pad);
            return false;
        }
    }
    return true;
}

// Score-derived upper-staff excerpt, truncated at 30,000 ms.
// Note onset times are preserved; the isolated E6 is lowered to E5.
struct PhraseEvent {
    uint32_t atMs;
    uint8_t pad;
    uint8_t velocity;
};

constexpr PhraseEvent PHRASE[] = {
    {    0, 10,  62}, {  208,  9,  62}, {  417, 10,  62},
    {  625,  9,  62}, {  833, 10,  62}, { 1042,  6,  62},
    { 1250,  8,  62}, { 1458,  7,  62}, { 1667,  5,  62},
    { 2292,  0,  62}, { 2500,  1,  62}, { 2708,  5,  62},
    { 2917,  6,  62}, { 3542,  1,  62}, { 3750,  4,  62},
    { 3958,  6,  62}, { 4167,  7,  62}, { 4792,  1,  62},
    { 5000, 10,  62}, { 5208,  9,  62}, { 5417, 10,  62},
    { 5625,  9,  62}, { 5833, 10,  62}, { 6042,  6,  62},
    { 6250,  8,  62}, { 6458,  7,  62}, { 6667,  5,  62},
    { 7292,  0,  62}, { 7500,  1,  62}, { 7708,  5,  62},
    { 7917,  6,  62}, { 8542,  1,  62}, { 8750,  7,  62},
    { 8958,  6,  62}, { 9167,  5,  62}, {10000, 10,  62},
    {10208,  9,  62}, {10417, 10,  62}, {10625,  9,  62},
    {10833, 10,  62}, {11042,  6,  62}, {11250,  8,  62},
    {11458,  7,  62}, {11667,  5,  62}, {12292,  0,  62},
    {12500,  1,  62}, {12708,  5,  62}, {12917,  6,  62},
    {13542,  1,  62}, {13750,  4,  62}, {13958,  6,  62},
    {14167,  7,  62}, {14792,  1,  62}, {15000, 10,  62},
    {15208,  9,  62}, {15417, 10,  62}, {15625,  9,  62},
    {15833, 10,  62}, {16042,  6,  62}, {16250,  8,  62},
    {16458,  7,  62}, {16667,  5,  62}, {17292,  0,  62},
    {17500,  1,  62}, {17708,  5,  62}, {17917,  6,  62},
    {18542,  1,  62}, {18750,  7,  62}, {18958,  6,  62},
    {19167,  5,  62}, {19792,  6,  62}, {20000,  7,  62},
    {20208,  8,  62}, {20417, 10,  62}, {21042,  3,  62},
    {21250, 11,  62}, {21458, 10,  62}, {21667,  8,  62},
    {22292,  2,  62}, {22500, 10,  62}, {22708,  8,  62},
    {22917,  7,  62}, {23542,  1,  62}, {23750,  8,  62},
    {23958,  7,  62}, {24167,  6,  62}, {24792,  1,  62},
    {25000, 10,  62}, {25208,  1,  62}, {25417, 10,  62},
    {25625, 10,  62}, {25833, 10,  62}, {26042,  9,  62},
    {26250, 10,  62}, {26458,  9,  62}, {26667, 10,  62},
    {26875,  9,  62}, {27083, 10,  62}, {27292,  9,  62},
    {27500, 10,  62}, {27708,  9,  62}, {27917, 10,  62},
    {28125,  9,  62}, {28333, 10,  62}, {28542,  6,  62},
    {28750,  8,  62}, {28958,  7,  62}, {29167,  5,  62},
    {29792,  0,  62}
};

constexpr size_t PHRASE_EVENT_COUNT = sizeof(PHRASE) / sizeof(PHRASE[0]);
uint32_t phraseStartedAt = 0;
size_t nextEventIndex = 0;
uint32_t droppedEvents = 0;
bool phraseReportPrinted = false;

static void haltWithError(const char *message)
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

    // mode1bit=false selects the configured 4-bit SD_MMC bus.
    return SD_MMC.begin("/sdcard", false);
}

void playDueEvents(uint32_t now)
{
    // Play this phrase exactly once. Keep timing anchored to phrase start,
    // and submit all due events in order if the application loop is late.
    while (nextEventIndex < PHRASE_EVENT_COUNT) {
        const PhraseEvent &event = PHRASE[nextEventIndex];
        const uint32_t dueAt = phraseStartedAt + event.atMs;
        if ((int32_t)(now - dueAt) < 0) {
            break;
        }

        if (!handpan.noteOn(event.pad, event.velocity)) {
            ++droppedEvents;
        }
        ++nextEventIndex;
    }
}


void setup()
{
    Serial.begin(115200);
    delay(250);

    Serial.println();
    Serial.println("======================================");
    Serial.println(" Elisa's Hand Pan — one-shot WAV recording");
    Serial.printf(" Phrase: %.1f seconds; note events: %u\n",
                  PHRASE_DURATION_MS / 1000.0f, (unsigned)PHRASE_EVENT_COUNT);
    Serial.printf(" Recording: %s.wav (%lu ms, stereo PCM16)\n",
                  RECORD_BASE_PATH, (unsigned long)RECORD_DURATION_MS);
    Serial.println("======================================");

    if (!mountSD()) {
        haltWithError("SD_MMC mount failed. Check the card, DIP switches and pin configuration.");
    }

    // Match the recording and resonator coefficients to the I2S output rate.
    if (!audioOut.setSampleRate((float)OUTPUT_SAMPLE_RATE)) {
        haltWithError("Could not configure the I2S sample rate.");
    }

    // Adds a little harmonic roughness without introducing another resonator.
    handpan.setSaturation(0.25f);
    if (!configureHandpan()) {
        Serial.println("ERROR: Could not configure the handpan pad bank");
        while (true) delay(1000);
    }

    hall.roomsize(0.68f);
    hall.damping(0.46f);
    const bool hallReady = hall.begin();
    if (!hallReady) {
        Serial.println("WARNING: Freeverb allocation failed; continuing without wet return");
    }
    dryWetMixer.gain(0, 0.72f);
    dryWetMixer.gain(1, hallReady ? 0.14f : 0.0f);

    audioOut.setChannelCount(codec.channels());
    AudioMemory(32);

    bool codecReady = false;
#if defined(AUDIO_CODEC_ES8311)
    codecReady = codec.enable(OUTPUT_SAMPLE_RATE);
#else
    codecReady = codec.enable();
#endif
    if (!codecReady) {
        Serial.println("ERROR: Audio codec initialization failed");
        while (true) delay(1000);
    }
    codec.volume(0.2f);

    // begin() creates /ElisasHandPan.tmp. stop() finalizes it as .wav.
    // Existing .tmp/.wav files are never overwritten automatically.
    if (!recorder.begin(SD_MMC, RECORD_BASE_PATH, OUTPUT_SAMPLE_RATE, 2,
                        AudioRecordFile::CORE_UNPINNED)) {
        haltWithError("Could not start recording. Check PSRAM, SD space, and remove an old /ElisasHandPan.wav or .tmp file before retrying.");
    }

    delay(30); // Let the writer task settle before the first note.
    phraseStartedAt = millis();
    nextEventIndex = 0;

    Serial.println("Recording started. Leave the board running until WAV finalization completes.");
}

void loop()
{
    const uint32_t now = millis();
    playDueEvents(now);

    if (nextEventIndex >= PHRASE_EVENT_COUNT && !phraseReportPrinted) {
        phraseReportPrinted = true;
        if (droppedEvents != 0) {
            Serial.printf("WARNING: %lu note events could not be queued.\n",
                          (unsigned long)droppedEvents);
        }
        Serial.println("Phrase finished; recording the resonator and reverb tails.");
    }

    if ((uint32_t)(now - phraseStartedAt) >= RECORD_DURATION_MS) {
        if (recorder.stop()) {
            Serial.printf("Recording complete: %s.wav on the SD card.\n",
                          RECORD_BASE_PATH);
        } else {
            Serial.printf("Recording finalization failed (error=%s). Check %s.tmp for recovery.\n",
                          recorder.hasError() ? "yes" : "no", RECORD_BASE_PATH);
        }

        // Do not start a second take. Reboot the board to record another one.
        while (true) {
            delay(1000);
        }
    }
}
