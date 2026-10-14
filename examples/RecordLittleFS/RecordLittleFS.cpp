/*
 * AudioOutputRecord example: write one short sine ping to LittleFS as WAV.
 *
 * This example uses the library's default 44.1 kHz audio graph and records
 * mono, 16-bit PCM. It does not require an audio codec or I2S output.
 *
 * LittleFS.begin(false) deliberately avoids automatic formatting. If the
 * LittleFS partition is new/unformatted and contains no data to preserve,
 * set FORMAT_LITTLEFS_ON_MOUNT_FAILURE to true for the first run.
 */

#include <Arduino.h>
#include <LittleFS.h>
#include <Audio.h>

constexpr uint32_t RECORD_SAMPLE_RATE = AUDIO_SAMPLE_RATE_EXACT;
constexpr bool FORMAT_LITTLEFS_ON_MOUNT_FAILURE = false;

AudioSynthWaveformSine pingSine;
AudioEffectEnvelope pingEnvelope;
AudioOutputRecord recorder;

AudioConnection sineToEnvelope(pingSine, pingEnvelope);
AudioConnection envelopeToRecorder(pingEnvelope, recorder);

static void haltWithError(const char *message) {
    Serial.println(message);
    while (true) {
        delay(1000);
    }
}

void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println();
    Serial.println("LittleFS WAV recorder example");

    if (!LittleFS.begin(FORMAT_LITTLEFS_ON_MOUNT_FAILURE)) {
        haltWithError("LittleFS mount failed. Check the partition or format it explicitly.");
    }

    // Audio graph and the shared audio-block pool.
    AudioMemory(12);

    // A short, decaying 880 Hz sine tone: a simple "ping".
    pingSine.frequency(880.0f);
    pingSine.amplitude(0.7f);

    pingEnvelope.attack(3.0f);
    pingEnvelope.hold(8.0f);
    pingEnvelope.decay(180.0f);
    pingEnvelope.sustain(0.0f);
    pingEnvelope.release(60.0f);

    // Creates /ping.tmp. On successful stop(), it becomes /ping.wav.
    // The PSRAM ring buffer defaults to 512 KiB.
    if (!recorder.begin(LittleFS, "/ping", RECORD_SAMPLE_RATE, 1,
                        AudioOutputRecord::CORE_UNPINNED)) {
        haltWithError("Could not start recorder. Check PSRAM and LittleFS.");
    }

    delay(30); // let recording and the writer task settle

    pingEnvelope.noteOn();
    delay(240);
    pingEnvelope.noteOff();
    delay(120); // retain the end of the envelope in the recording

    if (recorder.stop()) {
        Serial.println("Saved /ping.wav to LittleFS");
    } else {
        Serial.printf("Recording failed; error=%s. Check /ping.tmp for recovery.\n",
                      recorder.hasError() ? "yes" : "no");
    }
}

void loop() {
    delay(1000);
}
