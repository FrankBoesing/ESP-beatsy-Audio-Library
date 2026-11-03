/*
 * AudioSynthWavetableV2 real-SF2 benchmark -> USB or I2S output.
 *
 * Instrument: Airfont 340, Detuned EP2 (Bank 8, Program 5).
 * Every tested note in MIDI range 48..79 should start four mono sample layers.
 * The benchmark sweeps 1/2/4/8/16/32 notes, up to 128 sample voices.
 *
 * Each sweep is run twice with identical note/velocity sequences:
 *   FLASH: generated PROGMEM PCM arrays are read directly from mapped flash.
 *   PSRAM: each unique PCM array is copied once to PSRAM; sample/zone/instrument
 *          descriptors remain in internal RAM to isolate PCM fetch placement.
 * The order alternates each cycle (FLASH/PSRAM, then PSRAM/FLASH) to reduce
 * systematic order bias. The PCM copy includes interpolation guard frames.
 *
 * WAVETABLE_V2_TEST_NOTE_COUNT sets the maximum stage; default is 32.
 * Smaller values produce shorter sweeps. Run the same build on ESP32-S3/P4
 * and at the same sample rate when comparing memory placement.
 *
 * PlatformIO:
 *   ESP32-S3 USB: env:freenove_esp32_s3_display_USBAUDIO (48 kHz UAC1)
 *   ESP32-S3 I2S: env:freenove_esp32_s3_display (44.1/48 kHz, ES8311)
 *   ESP32-P4 I2S: env:esp32-p4 (44.1/48 kHz, ES8311)
 *
 * For I2S builds, define WAVETABLE_V2_TEST_RATE=44100 or 48000 to select the
 * output rate before AudioMemory() starts the I2S clock. USB audio is currently
 * configured only for the ESP32-S3 UAC1 test environment at 48 kHz.
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
#include <esp_cpu.h>
#include <esp_heap_caps.h>
#include <new>
#include <stddef.h>
#include <string.h>

#include "detuned_ep2.h"

// Maximum note count for the automatic 1/2/4/8/16/32-note sweep.
#ifndef WAVETABLE_V2_TEST_NOTE_COUNT
#define WAVETABLE_V2_TEST_NOTE_COUNT 32
#endif

// Set to 1 to enable periodic console load reports during measurement windows.
// Keep disabled for timing runs to minimize diagnostic I/O.
#ifndef WAVETABLE_V2_TEST_VERBOSE
#define WAVETABLE_V2_TEST_VERBOSE 0
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

// The generated header contains 50 zone sample descriptors. Some descriptors
// share identical PCM pointers, so copy each distinct PCM allocation only once.
using SampleData = AudioSynthWavetableV2::sample_data;
using ZoneData = AudioSynthWavetableV2::zone_data;
using InstrumentData = AudioSynthWavetableV2::instrument_data;

constexpr size_t SOURCE_SAMPLE_COUNT =
    sizeof(SoundFontInstrumentSamples) / sizeof(SoundFontInstrumentSamples[0]);
constexpr size_t SOURCE_ZONE_COUNT =
    sizeof(SoundFontInstrumentZones) / sizeof(SoundFontInstrumentZones[0]);
constexpr size_t MAX_UNIQUE_PCM_COPIES = SOURCE_SAMPLE_COUNT * 2U;

struct PcmCopyEntry {
    const int16_t *source;
    int16_t *psram;
    uint32_t frames;
};

static PcmCopyEntry psramPcmCopies[MAX_UNIQUE_PCM_COPIES] = {};
static size_t psramPcmCopyCount = 0;
static size_t psramPcmBytes = 0;
static const char *psramCopyError = nullptr;
static bool psramInstrumentReady = false;
static InstrumentData *psramInstrument = nullptr;

// Keep descriptor objects in internal RAM. Only PCM sample storage moves to
// PSRAM, isolating sample-data fetch placement from descriptor access.
alignas(SampleData) static uint8_t psramSampleStorage[sizeof(SampleData) * SOURCE_SAMPLE_COUNT];
alignas(ZoneData) static uint8_t psramZoneStorage[sizeof(ZoneData) * SOURCE_ZONE_COUNT];
alignas(InstrumentData) static uint8_t psramInstrumentStorage[sizeof(InstrumentData)];

static void freePsramPcmCopies() {
    for (size_t i = 0; i < psramPcmCopyCount; ++i) {
        if (psramPcmCopies[i].psram) heap_caps_free(psramPcmCopies[i].psram);
        psramPcmCopies[i] = {};
    }
    psramPcmCopyCount = 0;
    psramPcmBytes = 0;
}

static const int16_t *copyPcmToPsram(const int16_t *source, uint32_t frames) {
    if (!source || frames == 0) {
        psramCopyError = "Invalid source PCM pointer or frame count.";
        return nullptr;
    }

    for (size_t i = 0; i < psramPcmCopyCount; ++i) {
        if (psramPcmCopies[i].source == source) {
            if (psramPcmCopies[i].frames != frames) {
                psramCopyError = "One PCM pointer is used with inconsistent frame counts.";
                return nullptr;
            }
            return psramPcmCopies[i].psram;
        }
    }

    if (psramPcmCopyCount >= MAX_UNIQUE_PCM_COPIES) {
        psramCopyError = "The unique PCM pointer table is full.";
        return nullptr;
    }

    const size_t bytes = (size_t)frames * sizeof(int16_t);
    int16_t *destination = (int16_t *)heap_caps_malloc(
        bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!destination) {
        psramCopyError = "PSRAM allocation failed while copying PCM data.";
        return nullptr;
    }

    // PROGMEM on ESP32 is memory-mapped flash, so memcpy can read the source
    // normally. Copy every frame, including the interpolation guard frame.
    memcpy(destination, source, bytes);
    if (memcmp(destination, source, bytes) != 0) {
        heap_caps_free(destination);
        psramCopyError = "PCM data verification failed after the PSRAM copy.";
        return nullptr;
    }

    PcmCopyEntry &entry = psramPcmCopies[psramPcmCopyCount++];
    entry.source = source;
    entry.psram = destination;
    entry.frames = frames;
    psramPcmBytes += bytes;
    return destination;
}

static bool buildPsramInstrument() {
    if (psramInstrumentReady) return true;
    psramCopyError = nullptr;
    freePsramPcmCopies();

    SampleData *copiedSamples = (SampleData *)psramSampleStorage;
    for (size_t i = 0; i < SOURCE_SAMPLE_COUNT; ++i) {
        const SampleData &source = SoundFontInstrumentSamples[i];
        const int16_t *left = copyPcmToPsram(source.SAMPLE, source.SAMPLE_COUNT);
        if (!left) goto fail;

        const int16_t *right = nullptr;
        if (source.SAMPLE_RIGHT) {
            right = copyPcmToPsram(source.SAMPLE_RIGHT, source.SAMPLE_COUNT);
            if (!right) goto fail;
        }

        new (&copiedSamples[i]) SampleData(
            left,
            source.SAMPLE_COUNT,
            source.PER_HERTZ_PHASE_INCREMENT,
            source.MAX_PHASE,
            source.LOOP_MODE,
            source.LOOP_PHASE_END,
            source.LOOP_PHASE_LENGTH,
            source.INITIAL_ATTENUATION_SCALAR,
            source.DELAY_COUNT,
            source.ATTACK_COUNT,
            source.HOLD_COUNT,
            source.DECAY_COUNT,
            source.RELEASE_COUNT,
            source.SUSTAIN_MULT,
            right);
    }

    {
        ZoneData *copiedZones = (ZoneData *)psramZoneStorage;
        for (size_t i = 0; i < SOURCE_ZONE_COUNT; ++i) {
            const ZoneData &source = SoundFontInstrumentZones[i];
            const ptrdiff_t sampleIndex = source.SAMPLE - SoundFontInstrumentSamples;
            if (sampleIndex < 0 || (size_t)sampleIndex >= SOURCE_SAMPLE_COUNT) {
                psramCopyError = "A zone refers to a sample descriptor outside the sample table.";
                goto fail;
            }
            new (&copiedZones[i]) ZoneData(
                source.KEY_LOW,
                source.KEY_HIGH,
                source.VELOCITY_LOW,
                source.VELOCITY_HIGH,
                copiedSamples[sampleIndex]);
        }

        psramInstrument = new (psramInstrumentStorage)
            InstrumentData(copiedZones, SOURCE_ZONE_COUNT);
    }

    psramInstrumentReady = true;
    return true;

fail:
    freePsramPcmCopies();
    psramInstrument = nullptr;
    psramInstrumentReady = false;
    return false;
}

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
    if (WAVETABLE_V2_TEST_VERBOSE) {
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
    } else {
        // The constant false branch is optimized away; no polling or
        // diagnostic work remains in timing builds.
        delay(durationMs);
    }
}

static void playStressChord(uint8_t noteCount) {
    uint8_t startedNotes = 0;
    uint16_t startedSamples = 0;
    uint8_t minimumLayers = UINT8_MAX;
    uint8_t maximumLayers = 0;
    uint8_t layerDistribution[5] = {};

    for (uint8_t i = 0; i < noteCount; ++i) {
        const int note = TEST_FIRST_NOTE + i;
        const uint8_t layers = synth.noteOn(note, TEST_VELOCITY);

        if (layers != 0) ++startedNotes;
        startedSamples += layers;
        if (layers < minimumLayers) minimumLayers = layers;
        if (layers > maximumLayers) maximumLayers = layers;
        ++layerDistribution[layers <= 4 ? layers : 4];
    }

    consolePrint("Started notes: ");
    consolePrint((uint32_t)startedNotes);
    consolePrint(", active sample layers: ");
    consolePrint((uint32_t)startedSamples);
    consolePrint(", layers/note min..max: ");
    consolePrint((uint32_t)(minimumLayers == UINT8_MAX ? 0 : minimumLayers));
    consolePrint("..");
    consolePrint((uint32_t)maximumLayers);
    consolePrintln(" (expected 4).");

    consolePrint("Layer distribution 0/1/2/3/4: ");
    for (uint8_t i = 0; i <= 4; ++i) {
        if (i) consolePrint("/");
        consolePrint((uint32_t)layerDistribution[i]);
    }
    consolePrintln("");
}

static void printBenchmarkRow(const char *memoryMode, const char *phase,
                              uint8_t requestedNotes) {
    consolePrint("BENCH,");
    consolePrint(memoryMode);
    consolePrint(",");
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

static void runBenchmarkStage(const char *memoryMode, uint8_t noteCount) {
    synth.allSoundOff();
    delay(100);

    AudioStream::AudioProcessorUsageTotalMaxReset();
    synth.AudioProcessorUsageMaxReset();
    AudioMemoryUsageMaxReset();

    consolePrintln("");
    consolePrint("===== ");
    consolePrint(memoryMode);
    consolePrint(" BENCHMARK: ");
    consolePrint((uint32_t)noteCount);
    consolePrintln(" notes =====");
    playStressChord(noteCount);

    // Let note-on setup settle before the measured sustain interval.
    delay(200);
    AudioStream::AudioProcessorUsageTotalMaxReset();
    synth.AudioProcessorUsageMaxReset();
    AudioMemoryUsageMaxReset();
    monitorFor(3000);
    printBenchmarkRow(memoryMode, "sustain", noteCount);
    consolePrint(memoryMode);
    printPhasePeak(" Sustain");

    // Measure the release envelope separately from the sustained workload.
    AudioStream::AudioProcessorUsageTotalMaxReset();
    synth.AudioProcessorUsageMaxReset();

    // Keep instrumentation to two cycle-counter reads around allNotesOff().
    // Defer all console output until the release measurement window has ended.
    const uint32_t cpuMHz = getCpuFrequencyMhz();
    const uint32_t noteOffStartCycles = esp_cpu_get_cycle_count();
    synth.allNotesOff();
    const uint32_t noteOffCycles = esp_cpu_get_cycle_count() - noteOffStartCycles;

    monitorFor(1200);
    printBenchmarkRow(memoryMode, "release", noteCount);
    consolePrint(memoryMode);
    printPhasePeak(" Release");

    const uint32_t noteOffUs = cpuMHz != 0 ? noteOffCycles / cpuMHz : 0;
    consolePrint("NOTE_OFF,");
    consolePrint(memoryMode);
    consolePrint(",");
    consolePrint(noteOffCycles);
    consolePrint(",");
    consolePrint(cpuMHz);
    consolePrint(",");
    consolePrintln(noteOffUs);

    // Hard-stop residual voices before the next stage, then leave a short gap.
    synth.allSoundOff();
    delay(150);
}

static void runBenchmarkSweep(const char *memoryMode,
                              const InstrumentData &selectedInstrument) {
    synth.allSoundOff();
    delay(100);
    if (!synth.setInstrument(selectedInstrument)) {
        haltWithError("Could not activate the selected benchmark instrument.");
    }

    consolePrintln("");
    consolePrint("========== MEMORY MODE: ");
    consolePrint(memoryMode);
    consolePrintln(" ==========");

    uint8_t noteCount = 1;
    for (;;) {
        runBenchmarkStage(memoryMode, noteCount);
        if (noteCount >= TEST_NOTE_COUNT) break;

        uint16_t nextCount = (uint16_t)noteCount * 2U;
        if (nextCount > TEST_NOTE_COUNT) nextCount = TEST_NOTE_COUNT;
        noteCount = (uint8_t)nextCount;
    }
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
    consolePrintln("AudioSynthWavetableV2 real-SF2 memory benchmark.");
    consolePrintln("Instrument: Airfont 340 - Detuned EP2 (Bank 8, Program 5).");
    consolePrintln("MIDI notes 48..79 at velocity 112; expected four mono layers per note.");
    consolePrint("Configured notes: ");
    consolePrint((uint32_t)TEST_NOTE_COUNT);
    consolePrint(", maximum sample voices: ");
    consolePrintln((uint32_t)(TEST_NOTE_COUNT * AudioSynthWavetableV2::MAX_SAMPLE_LAYERS_PER_NOTE));
#if defined(AUDIO_OUTPUT_USB)
    consolePrintln("USB output: mono SF2 mix mirrored to stereo, 16-bit PCM.");
#else
    consolePrintln("I2S output: mono SF2 mix mirrored to stereo.");
#endif
#if defined(AUDIO_OUTPUT_USB)
    consolePrintln("Select the ESP32 USB microphone in Audacity and start recording.");
#else
    consolePrintln("Record the codec output through line-in or an external recorder.");
#endif
    consolePrintln("BENCH CSV: BENCH,memory_mode,phase,rate_hz,requested_notes,active_notes,active_layers,node_us,node_peak_us,graph_us,graph_peak_us,blocks,blocks_peak");
    consolePrintln("NOTE_OFF CSV: NOTE_OFF,memory_mode,cycles,cpu_mhz,elapsed_us");
#if WAVETABLE_V2_TEST_VERBOSE
    consolePrintln("Periodic diagnostic output enabled.");
#else
    consolePrintln("Periodic diagnostic output disabled for timing accuracy.");
#endif

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

    if (!synth.setInstrument(SoundFontInstrumentInstrument)) {
        haltWithError("Invalid Detuned EP2 V2 instrument data.");
    }

    const size_t psramFreeBefore = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (buildPsramInstrument()) {
        consolePrint("PSRAM PCM copy ready: ");
        consolePrint((uint32_t)psramPcmCopyCount);
        consolePrint(" unique arrays, ");
        consolePrint((uint32_t)psramPcmBytes);
        consolePrint(" bytes; free PSRAM ");
        consolePrint((uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        consolePrintln(" bytes.");
    } else {
        consolePrint("PSRAM copy unavailable: ");
        consolePrintln(psramCopyError ? psramCopyError : "unknown allocation/copy error");
        consolePrint("PSRAM free before copy: ");
        consolePrint((uint32_t)psramFreeBefore);
        consolePrintln(" bytes. FLASH benchmark will still run.");
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
    // Alternate order on successive cycles to reduce systematic order bias.
    static bool psramFirst = false;

    if (psramInstrumentReady && psramFirst) {
        runBenchmarkSweep("PSRAM", *psramInstrument);
        runBenchmarkSweep("FLASH", SoundFontInstrumentInstrument);
    } else {
        runBenchmarkSweep("FLASH", SoundFontInstrumentInstrument);
        if (psramInstrumentReady) {
            runBenchmarkSweep("PSRAM", *psramInstrument);
        }
    }

    psramFirst = !psramFirst;
    consolePrintln("FLASH/PSRAM comparison complete. Repeating in 1 second.");
    delay(1000);
}
