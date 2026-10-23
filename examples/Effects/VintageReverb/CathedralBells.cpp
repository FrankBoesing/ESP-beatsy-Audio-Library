/*
 * Cathedral Bells - standalone VERBtrix demonstration.
 *
 * No I2S input is used. A small procedural modal synthesizer generates
 * slow bell-like strikes, which are sent through AudioEffectVintageReverb
 * to stereo I2S output.
 *
 * Original reverb DSP core by Bas Scheffers, MIT license.
 */

#include <Audio.h>

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

AudioConnection bellsToLeft(bells, 0, cathedral, 0);
AudioConnection bellsToRight(bells, 0, cathedral, 1);
AudioConnection reverbToLeft(cathedral, 0, audioOut, 0);
AudioConnection reverbToRight(cathedral, 1, audioOut, 1);

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

    phraseStartMs = millis();
    nextBellEvent = 0;
}

void loop()
{
    const uint32_t now = millis();
    playBellEvents(now);

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
