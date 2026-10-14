/*
 * AudioRecordFile example: write one short sine ping to an SD card as WAV.
 *
 * This example uses the library's default 44.1 kHz audio graph and records
 * mono, 16-bit PCM. It does not require an audio codec or I2S output.
 *
 * When PIN_SD_CS and the SPI pin macros are supplied by platformio.ini, they
 * are used below. Otherwise SD.begin() uses the board's default SPI pins.
 * Adjust the pins to match your SD-card wiring.
 */

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <Audio.h>

constexpr uint32_t RECORD_SAMPLE_RATE = AUDIO_SAMPLE_RATE_EXACT;

AudioSynthWaveformSine pingSine;
AudioEffectEnvelope pingEnvelope;
AudioRecordFile recorder;

AudioConnection sineToEnvelope(pingSine, pingEnvelope);
AudioConnection envelopeToRecorder(pingEnvelope, recorder);

static void haltWithError(const char *message) {
    Serial.println(message);
    while (true) {
        delay(1000);
    }
}

static bool mountSD() {
#if defined(PIN_SD_CS) && defined(PIN_SPI_MOSI) && defined(PIN_SPI_MISO) && defined(PIN_SPI_SCK)
    SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_SD_CS);
    return SD.begin(PIN_SD_CS, SPI);
#else
    return SD.begin();
#endif
}

void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println();
    Serial.println("SD-card WAV recorder example");

    if (!mountSD()) {
        haltWithError("SD mount failed. Check card, wiring and SPI pin configuration.");
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
    if (!recorder.begin(SD, "/ping", RECORD_SAMPLE_RATE, 1,
                        AudioRecordFile::CORE_UNPINNED)) {
        haltWithError("Could not start recorder. Check PSRAM, SD card and filenames.");
    }

    delay(30); // let recording and the writer task settle

    pingEnvelope.noteOn();
    delay(240);
    pingEnvelope.noteOff();
    delay(120); // retain the end of the envelope in the recording

    if (recorder.stop()) {
        Serial.println("Saved /ping.wav to SD card");
    } else {
        Serial.printf("Recording failed; error=%s. Check /ping.tmp for recovery.\n",
                      recorder.hasError() ? "yes" : "no");
    }
}

void loop() {
    delay(1000);
}
