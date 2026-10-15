#include <Arduino.h>
#include <SD_MMC.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <Audio.h>

// -----------------------------------------------------------------------------
// Diesel engine sample player
//
// Source sample:
//   "Diesel Start-Idle-Revs MZ000004.wav" by DAVESTALKER
//   https://freesound.org/people/DAVESTALKER/sounds/443319/
//   License: CC0
//
// Copy the downloaded, unmodified PCM WAV file to the SD card as /diesel.wav.
// Required format: PCM, mono, 44.1 kHz, 16 bit.
//
// The segment boundaries below are INITIAL GUESSES, not verified timings.
// Adjust them after listening to the selected WAV on your setup.
// -----------------------------------------------------------------------------

namespace {
constexpr uint32_t SAMPLE_RATE = 44100;
constexpr uint32_t REFERENCE_RPM = 800;

// Initial estimates only; adjust after listening to the actual source file.
constexpr uint32_t START_END_MS = 3800;
constexpr uint32_t IDLE_START_MS = 3800;
constexpr uint32_t IDLE_END_MS = 10500;
constexpr uint32_t REV_START_MS = 11500;
constexpr uint32_t REV_END_MS = 17000;

constexpr uint16_t MIN_RPM = 400;
constexpr uint16_t MAX_RPM = 2400;
constexpr uint16_t RPM_STEP = 100;

enum class PlaybackMode : uint8_t {
    Stopped,
    IdleLoop,
    StartOnce,
    RevOnce
};

// The serial/control task and audio update task only share command data under
// this short lock. Sample-position and interpolation state belong to update().
portMUX_TYPE commandMux = portMUX_INITIALIZER_UNLOCKED;

uint16_t currentRpm = REFERENCE_RPM;

uint16_t readLE16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

uint32_t readLE32(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

bool readExact(File &file, uint8_t *dst, size_t bytes) {
    return file.read(dst, bytes) == bytes;
}

uint32_t msToFrame(uint32_t ms) {
    return (uint32_t)(((uint64_t)ms * SAMPLE_RATE) / 1000U);
}
} // namespace

class AudioPlayDiesel : public AudioStream {
public:
    AudioPlayDiesel() : AudioStream(0, nullptr) {}

    bool loadWav(fs::FS &fs, const char *path) {
        File file = fs.open(path, FILE_READ);
        if (!file) {
            Serial.printf("ERROR: Cannot open %s\n", path);
            return false;
        }

        uint8_t riff[12];
        if (!readExact(file, riff, sizeof(riff)) ||
            memcmp(riff, "RIFF", 4) != 0 ||
            memcmp(riff + 8, "WAVE", 4) != 0) {
            Serial.println("ERROR: Not a RIFF/WAVE file");
            file.close();
            return false;
        }

        bool haveFormat = false;
        uint16_t audioFormat = 0;
        uint16_t channels = 0;
        uint16_t bitsPerSample = 0;
        uint32_t sampleRate = 0;
        uint32_t dataBytes = 0;
        uint32_t dataOffset = 0;

        while (file.position() + 8 <= file.size()) {
            uint8_t chunkHeader[8];
            if (!readExact(file, chunkHeader, sizeof(chunkHeader))) {
                break;
            }

            const uint32_t chunkSize = readLE32(chunkHeader + 4);
            const uint32_t payloadOffset = (uint32_t)file.position();

            if (memcmp(chunkHeader, "fmt ", 4) == 0) {
                uint8_t fmt[16];
                if (chunkSize < sizeof(fmt) ||
                    !readExact(file, fmt, sizeof(fmt))) {
                    Serial.println("ERROR: Invalid WAV fmt chunk");
                    file.close();
                    return false;
                }
                audioFormat = readLE16(fmt + 0);
                channels = readLE16(fmt + 2);
                sampleRate = readLE32(fmt + 4);
                bitsPerSample = readLE16(fmt + 14);
                haveFormat = true;
            } else if (memcmp(chunkHeader, "data", 4) == 0) {
                dataOffset = payloadOffset;
                dataBytes = chunkSize;
            }

            // Chunks are padded to an even byte boundary.
            if (!file.seek((uint32_t)(payloadOffset + chunkSize + (chunkSize & 1U)))) {
                Serial.println("ERROR: Invalid WAV chunk length");
                file.close();
                return false;
            }
        }

        if (!haveFormat || dataBytes == 0 || audioFormat != 1 ||
            channels != 1 || sampleRate != SAMPLE_RATE ||
            bitsPerSample != 16 || (dataBytes & 1U) != 0) {
            Serial.println("ERROR: Need uncompressed PCM, mono, 44100 Hz, 16 bit WAV");
            file.close();
            return false;
        }

        int16_t *newSamples = static_cast<int16_t *>(
            heap_caps_malloc(dataBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (newSamples == nullptr) {
            // Fallback is useful on boards with enough internal RAM, but this
            // 26.6 s sample normally requires PSRAM.
            newSamples = static_cast<int16_t *>(
                heap_caps_malloc(dataBytes, MALLOC_CAP_8BIT));
        }
        if (newSamples == nullptr) {
            Serial.printf("ERROR: Cannot allocate %u bytes for the WAV sample\n",
                          (unsigned)dataBytes);
            file.close();
            return false;
        }

        if (!file.seek(dataOffset) ||
            file.read(reinterpret_cast<uint8_t *>(newSamples), dataBytes) != dataBytes) {
            Serial.println("ERROR: Could not read complete WAV data");
            heap_caps_free(newSamples);
            file.close();
            return false;
        }
        file.close();

        if (samples_ != nullptr) {
            heap_caps_free(samples_);
        }
        samples_ = newSamples;
        sampleCount_ = dataBytes / sizeof(int16_t);

        idleStart_ = msToFrame(IDLE_START_MS);
        idleEnd_ = msToFrame(IDLE_END_MS);
        startEnd_ = msToFrame(START_END_MS);
        revStart_ = msToFrame(REV_START_MS);
        revEnd_ = msToFrame(REV_END_MS);

        if (startEnd_ >= sampleCount_ || idleStart_ >= idleEnd_ ||
            idleEnd_ > sampleCount_ || revStart_ >= revEnd_ ||
            revEnd_ > sampleCount_) {
            Serial.println("ERROR: Edit the sample segment times near the top of DieselEngine.cpp");
            heap_caps_free(samples_);
            samples_ = nullptr;
            sampleCount_ = 0;
            return false;
        }

        Serial.printf("Loaded %s: %u samples (%.2f s), %u bytes\n",
                      path,
                      (unsigned)sampleCount_,
                      (double)sampleCount_ / SAMPLE_RATE,
                      (unsigned)dataBytes);
        return true;
    }

    void setRpm(uint16_t rpm) {
        if (rpm < MIN_RPM) rpm = MIN_RPM;
        if (rpm > MAX_RPM) rpm = MAX_RPM;
        currentRpm = rpm;
        targetRate_ = (float)rpm / (float)REFERENCE_RPM;
    }

    void playStart() {
        request(PlaybackMode::StartOnce, 0, startEnd_);
    }

    void playIdle() {
        request(PlaybackMode::IdleLoop, idleStart_, idleEnd_);
    }

    void playRev() {
        request(PlaybackMode::RevOnce, revStart_, revEnd_);
    }

    void stop() {
        request(PlaybackMode::Stopped, 0, 0);
    }

    void update() override {
        audio_block_t *block = allocate();
        if (block == nullptr) {
            return;
        }

        // Apply the latest control request at the audio-block boundary.
        uint32_t requestVersion;
        uint32_t requestStart;
        uint32_t requestEnd;
        PlaybackMode requestMode;

        portENTER_CRITICAL(&commandMux);
        requestVersion = requestVersion_;
        requestMode = requestedMode_;
        requestStart = requestedStart_;
        requestEnd = requestedEnd_;
        portEXIT_CRITICAL(&commandMux);

        if (requestVersion != seenRequestVersion_) {
            seenRequestVersion_ = requestVersion;
            mode_ = requestMode;
            rangeStart_ = requestStart;
            rangeEnd_ = requestEnd;
            position_ = (double)requestStart;
        }

        if (samples_ == nullptr || sampleCount_ == 0 ||
            mode_ == PlaybackMode::Stopped) {
            memset(block->data, 0, sizeof(block->data));
            transmit(block);
            release(block);
            return;
        }

        int16_t *out = block->data;
        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            if (position_ >= (double)rangeEnd_) {
                if (mode_ == PlaybackMode::StartOnce ||
                    mode_ == PlaybackMode::RevOnce) {
                    mode_ = PlaybackMode::IdleLoop;
                    rangeStart_ = idleStart_;
                    rangeEnd_ = idleEnd_;
                    position_ = (double)idleStart_;
                } else {
                    const uint32_t loopLength = rangeEnd_ - rangeStart_;
                    if (loopLength == 0) {
                        mode_ = PlaybackMode::Stopped;
                        out[i] = 0;
                        continue;
                    }
                    position_ = (double)rangeStart_ +
                                fmod(position_ - (double)rangeStart_,
                                     (double)loopLength);
                }
            }

            const uint32_t index0 = (uint32_t)position_;
            uint32_t index1 = index0 + 1;
            if (index1 >= rangeEnd_) {
                index1 = (mode_ == PlaybackMode::IdleLoop)
                             ? rangeStart_
                             : (rangeEnd_ - 1);
            }

            const float fraction = (float)(position_ - (double)index0);
            const int32_t s0 = samples_[index0];
            const int32_t s1 = samples_[index1];
            out[i] = (int16_t)(s0 + (int32_t)((s1 - s0) * fraction));

            // Smooth speed changes to avoid abrupt step changes in read rate.
            const float target = targetRate_;
            currentRate_ += (target - currentRate_) * 0.0015f;
            position_ += currentRate_;
        }

        transmit(block);
        release(block);
    }

private:
    void request(PlaybackMode mode, uint32_t start, uint32_t end) {
        portENTER_CRITICAL(&commandMux);
        requestedMode_ = mode;
        requestedStart_ = start;
        requestedEnd_ = end;
        ++requestVersion_;
        portEXIT_CRITICAL(&commandMux);
    }

    int16_t *samples_ = nullptr;
    uint32_t sampleCount_ = 0;

    uint32_t startEnd_ = 0;
    uint32_t idleStart_ = 0;
    uint32_t idleEnd_ = 0;
    uint32_t revStart_ = 0;
    uint32_t revEnd_ = 0;

    // These playback fields are accessed only by update().
    PlaybackMode mode_ = PlaybackMode::Stopped;
    double position_ = 0.0;
    float currentRate_ = 1.0f;
    uint32_t rangeStart_ = 0;
    uint32_t rangeEnd_ = 0;

    // Control requests are copied under commandMux.
    PlaybackMode requestedMode_ = PlaybackMode::Stopped;
    uint32_t requestedStart_ = 0;
    uint32_t requestedEnd_ = 0;
    uint32_t requestVersion_ = 0;
    uint32_t seenRequestVersion_ = 0;
    volatile float targetRate_ = 1.0f;
};

AudioPlayDiesel diesel;

AudioOutputI2S output({
    PIN_I2S_BLCK,
    PIN_I2S_WS,
    PIN_I2S_DOUT,
    PIN_I2S_MLCK
});

#if defined(AUDIO_CODEC_ES8388)
AudioControlES8388 codec({PIN_I2C_SDA, PIN_I2C_SCL, PIN_AMPLIFIER, PIN_AMPLIFIER_ACTIVE});
#elif defined(AUDIO_CODEC_ES8311)
AudioControlES8311 codec({PIN_I2C_SDA, PIN_I2C_SCL, PIN_AMPLIFIER, PIN_AMPLIFIER_ACTIVE});
#else
#error "AUDIO_CODEC not defined"
#endif

AudioConnection patchCordLeft(diesel, 0, output, 0);
AudioConnection patchCordRight(diesel, 0, output, 1);

static void printHelp() {
    Serial.println();
    Serial.println("Diesel engine controls:");
    Serial.println("  s = replay engine start, then idle loop");
    Serial.println("  i = idle loop");
    Serial.println("  r = play rev segment once, then idle loop");
    Serial.println("  + / - = RPM +/- 100");
    Serial.println("  0 = reset to reference idle RPM");
    Serial.println("  x = stop");
    Serial.printf("Current RPM: %u\n", currentRpm);
}

void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println();
    Serial.println("======================================");
    Serial.println(" Diesel Engine Sample Player");
    Serial.println("======================================");

    if (!SD_MMC.begin("/sdcard", true)) {
        Serial.println("ERROR: SD_MMC initialization failed");
        while (true) delay(1000);
    }

    // Load the complete ~2.35 MB PCM sample into PSRAM before audio starts.
    if (!diesel.loadWav(SD_MMC, "/diesel.wav")) {
        Serial.println("Copy the CC0 source WAV to the card as /diesel.wav.");
        while (true) delay(1000);
    }

    AudioMemory(12);
    output.setChannelCount(codec.channels());

    if (!codec.enable()) {
        Serial.println("ERROR: Audio codec initialization failed");
        while (true) delay(1000);
    }
    codec.volume(0.65f);

    diesel.setRpm(REFERENCE_RPM);
    diesel.playStart();
    printHelp();
}

void loop() {
    while (Serial.available() > 0) {
        const char command = (char)Serial.read();
        switch (command) {
        case 's':
        case 'S':
            diesel.playStart();
            Serial.println("Playing start, then idle");
            break;
        case 'i':
        case 'I':
            diesel.playIdle();
            Serial.println("Idle loop");
            break;
        case 'r':
        case 'R':
            diesel.playRev();
            Serial.println("Playing rev segment, then idle");
            break;
        case '+':
            currentRpm = (currentRpm + RPM_STEP > MAX_RPM)
                             ? MAX_RPM
                             : (uint16_t)(currentRpm + RPM_STEP);
            diesel.setRpm(currentRpm);
            Serial.printf("RPM: %u\n", currentRpm);
            break;
        case '-':
            currentRpm = (currentRpm < MIN_RPM + RPM_STEP)
                             ? MIN_RPM
                             : (uint16_t)(currentRpm - RPM_STEP);
            diesel.setRpm(currentRpm);
            Serial.printf("RPM: %u\n", currentRpm);
            break;
        case '0':
            currentRpm = REFERENCE_RPM;
            diesel.setRpm(currentRpm);
            Serial.printf("RPM: %u (reference)\n", currentRpm);
            break;
        case 'x':
        case 'X':
            diesel.stop();
            Serial.println("Stopped");
            break;
        case '?':
            printHelp();
            break;
        default:
            break;
        }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
}
