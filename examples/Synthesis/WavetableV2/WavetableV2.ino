/*
 * AudioSynthWavetableV2 stress test -> USB microphone.
 *
 * Starts 32 notes with four matching sample layers each (up to 128 sample
 * voices) and sends the mono synth signal to both channels of AudioOutputUSB.
 * Record the ESP32 USB microphone input on the PC, e.g. with Audacity.
 *
 * PlatformIO: env:freenove_esp32_s3_display_USBAUDIO
 * The environment must enable AUDIO_OUTPUT_USB and configure 48 kHz UAC1.
 */

#if !defined(ARDUINO_USB_MODE) || ARDUINO_USB_MODE != 0 || \
    !defined(ARDUINO_USB_CDC_ON_BOOT) || ARDUINO_USB_CDC_ON_BOOT != 0
#error "WavetableV2 USB output requires TinyUSB mode and ARDUINO_USB_CDC_ON_BOOT=0."
#endif

#if !defined(AUDIO_OUTPUT_USB)
#error "Build this example with AUDIO_OUTPUT_USB enabled."
#endif

#include <Arduino.h>
#include <Audio.h>
#include <stdarg.h>
#include <stdio.h>

constexpr uint8_t TEST_NOTE_COUNT = AudioSynthWavetableV2::MAX_NOTES;
constexpr int TEST_FIRST_NOTE = 48;
constexpr int TEST_VELOCITY = 112;

AudioSynthWavetableV2 synth;
AudioOutputUSB usbOutput;
AudioConnection synthToUsbLeft(synth, 0, usbOutput, 0);
AudioConnection synthToUsbRight(synth, 0, usbOutput, 1);

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

static void consolePrintln(const char *message) {
    Serial.println(message);

    USBCDC &usbCdc = AudioUSBSerial();
    if (usbCdc) {
        usbCdc.println(message);
    }
}

static void consolePrintf(const char *format, ...) {
    char buffer[256];

    va_list args;
    va_start(args, format);
    const int length = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    if (length <= 0) {
        return;
    }

    Serial.print(buffer);

    USBCDC &usbCdc = AudioUSBSerial();
    if (usbCdc) {
        usbCdc.print(buffer);
    }
}

static void haltWithError(const char *message) {
    consolePrintln(message);
    while (true) {
        delay(1000);
    }
}

static void printLoad() {
    consolePrintf(
        "Audio CPU: graph %5.1f%% (max %5.1f%%), V2 %5.1f%% (max %5.1f%%), "
        "notes %u, sample voices %u, audio blocks %u/%u, USB configured %s\n",
        AudioStream::AudioProcessorUsageTotal(), AudioStream::AudioProcessorUsageTotalMax(),
        synth.AudioProcessorUsage(), synth.AudioProcessorUsageMax(),
        (unsigned)synth.activeNoteCount(), (unsigned)synth.activeSampleCount(),
        (unsigned)AudioMemoryUsage(), (unsigned)AudioMemoryUsageMax(),
        usbOutput.isConfigured() ? "yes" : "no");
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

static void playStressChord() {
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

    consolePrintf("Started %u notes and %u sample layers. Expected maximum: 32 / 128.\n",
                  (unsigned)startedNotes, (unsigned)startedSamples);
}

void setup() {
    // Keep UART0 available for the PlatformIO monitor. Diagnostics are mirrored
    // to the USBManager-owned CDC interface after AudioMemory() starts USB.
    Serial.begin(115200);

    // The managed USB device (including its CDC serial interface) is started
    // by AudioMemory(). No USB.begin(), USBAudioCard instance or loop() polling
    // is needed.
    AudioMemory(24);
    delay(300);

    consolePrintln("");
    consolePrintln("AudioSynthWavetableV2: 32 notes / up to 128 sample voices");
    consolePrintln("USB microphone output: stereo, 16-bit PCM.");
    consolePrintln("Select the ESP32 USB microphone in Audacity and start recording.");

    if (!usbOutput.isConfigured()) {
        haltWithError("Managed AudioOutputUSB initialization failed. Check the USB build environment.");
    }

    if (!synth.setInstrument(instrument)) {
        haltWithError("Invalid V2 instrument data.");
    }
    synth.amplitude(0.35f);

    consolePrintf("USB audio rate: %.0f Hz. Waiting 5 seconds for Audacity...\n",
                  AUDIO_SAMPLE_RATE_EXACT);
    delay(5000);

    consolePrintln("Stress test started. It repeats until the board is reset.");
}

void loop() {
    consolePrintln("Loop entered.");

    AudioStream::AudioProcessorUsageTotalMaxReset();
    synth.AudioProcessorUsageMaxReset();
    AudioMemoryUsageMaxReset();

    consolePrintln("Loop counters reset; starting noteOn calls.");
    playStressChord();

    // Keep the 32-note / 128-layer load active while the PC records USB audio.
    monitorFor(3000);

    synth.allNotesOff();
    consolePrintln("Note-Off sent; recording the release tails.");
    monitorFor(1200);

    // Leave a short gap before the next run. This makes it easy to identify
    // separate measurement cycles in the Audacity recording.
    delay(800);
}
