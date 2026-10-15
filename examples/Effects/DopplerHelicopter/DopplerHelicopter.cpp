#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <math.h>
#include <Audio.h>

// Copy a short, mono, 44.1 kHz / 16-bit PCM rotor loop to the SD card as
// /Helicopter.wav. The sample is preloaded into PSRAM before playback.

namespace {
constexpr char SAMPLE_FILE[] = "/Helicopter.wav";

constexpr float SPEED_OF_SOUND_MPS = 343.0f;
constexpr float HALF_SPAN_M = 180.0f;    // Far-right to far-left distance
constexpr float CLOSEST_DISTANCE_M = 35.0f;
constexpr uint32_t LEG_DURATION_MS = 15500; // One direction
constexpr float PI_F = 3.14159265358979323846f;

bool mountSD() {
#if defined(PIN_SD_CS) && defined(PIN_SPI_MOSI) && defined(PIN_SPI_MISO) && defined(PIN_SPI_SCK)
    SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_SD_CS);
    return SD.begin(PIN_SD_CS, SPI);
#else
    return SD.begin();
#endif
}
} // namespace

// Example-local mono-to-stereo panner. The loaded PCM buffer is mono, so the
// flight model controls its entire stereo position.
class AudioEffectDopplerPan : public AudioStream {
public:
    AudioEffectDopplerPan() : AudioStream(1, inputQueueArray) {}

    // pan: -1 = hard left, 0 = centre, +1 = hard right; gain: 0 ... 1.
    void setPosition(float pan, float gain) {
        if (pan < -1.0f) pan = -1.0f;
        if (pan > 1.0f) pan = 1.0f;
        if (gain < 0.0f) gain = 0.0f;
        if (gain > 1.0f) gain = 1.0f;

        portENTER_CRITICAL(&controlMux);
        targetPan = pan;
        targetGain = gain;
        portEXIT_CRITICAL(&controlMux);
    }

    void update() override {
        audio_block_t *input = receiveReadOnly(0);
        if (input == nullptr) {
            return;
        }

        audio_block_t *left = allocate();
        audio_block_t *right = allocate();
        if (left == nullptr || right == nullptr) {
            if (left != nullptr) release(left);
            if (right != nullptr) release(right);
            release(input);
            return;
        }

        float panTarget;
        float gainTarget;
        portENTER_CRITICAL(&controlMux);
        panTarget = targetPan;
        gainTarget = targetGain;
        portEXIT_CRITICAL(&controlMux);

        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            currentPan += (panTarget - currentPan) * 0.0015f;
            currentGain += (gainTarget - currentGain) * 0.0015f;

            // Equal-power panning keeps the perceived level steadier at centre.
            const float leftGain = sqrtf((1.0f - currentPan) * 0.5f) * currentGain;
            const float rightGain = sqrtf((1.0f + currentPan) * 0.5f) * currentGain;
            left->data[i] = (int16_t)(input->data[i] * leftGain);
            right->data[i] = (int16_t)(input->data[i] * rightGain);
        }

        transmit(left, 0);
        transmit(right, 1);
        release(left);
        release(right);
        release(input);
    }

private:
    audio_block_t *inputQueueArray[1] = {};
    portMUX_TYPE controlMux = portMUX_INITIALIZER_UNLOCKED;
    float targetPan = 1.0f;
    float targetGain = 0.0f;
    float currentPan = 1.0f;
    float currentGain = 0.0f;
};

AudioPlaySdWav wavLoader;
AudioSampleBuffer helicopterSample;
AudioPlayMemory helicopter;
AudioEffectDopplerPan pan;
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

AudioConnection patchCordSource(helicopter, 0, pan, 0);
AudioConnection patchCordLeft(pan, 0, output, 0);
AudioConnection patchCordRight(pan, 1, output, 1);

void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println();
    Serial.println("======================================");
    Serial.println(" Doppler Helicopter (PSRAM sample loop)");
    Serial.println("======================================");

    if (!mountSD()) {
        Serial.println("ERROR: SD (SPI) initialization failed. Check the card and platformio.ini pins.");
        while (true) delay(1000);
    }

    AudioMemory(16);

    // The whole decoded sample is loaded into PSRAM. Loading fails without
    // replacing an existing buffer if contiguous PSRAM or the reserve is insufficient.
    if (!wavLoader.loadToMemory(SD, SAMPLE_FILE, helicopterSample)) {
        Serial.printf("ERROR: Could not preload %s into PSRAM.\n", SAMPLE_FILE);
        Serial.println("Use a shorter mono PCM WAV (44.1/22.05/11.025 kHz, 8/16/24/32 bit).");
        while (true) delay(1000);
    }

    Serial.printf("Loaded %lu samples at %lu Hz (%u bytes in PSRAM)\n",
                  (unsigned long)helicopterSample.sampleCount(),
                  (unsigned long)helicopterSample.sampleRate(),
                  (unsigned)helicopterSample.sizeBytes());

    output.setChannelCount(codec.channels());
    if (!codec.enable()) {
        Serial.println("ERROR: Audio codec initialization failed");
        while (true) delay(1000);
    }
    codec.volume(0.65f);

    helicopter.setPlaybackRate(1.0f);
    pan.setPosition(1.0f, 0.0f);

    helicopter.setLoop(true);
    if (!helicopter.play(helicopterSample)) {
        Serial.println("ERROR: AudioPlayMemory rejected the loaded sample");
        while (true) delay(1000);
    }
}

void loop() {
    const uint32_t now = millis();
    const uint32_t cycleMs = 2U * LEG_DURATION_MS;
    const uint32_t cyclePosition = now % cycleMs;
    const bool flyingLeft = cyclePosition < LEG_DURATION_MS;
    const uint32_t legPosition = flyingLeft
                                     ? cyclePosition
                                     : cyclePosition - LEG_DURATION_MS;
    const float t = (float)legPosition / (float)LEG_DURATION_MS; // 0 ... 1
    const float angle = PI_F * t;

    // Cosine trajectory: starts and turns around at a distance, crossing the
    // listener midway through each leg. x > 0 is right; x < 0 is left.
    const float x = (flyingLeft ? 1.0f : -1.0f) * HALF_SPAN_M * cosf(angle);
    const float velocityX = (flyingLeft ? -1.0f : 1.0f) *
                            HALF_SPAN_M * PI_F /
                            (LEG_DURATION_MS / 1000.0f) * sinf(angle);

    const float distance = sqrtf(x * x + CLOSEST_DISTANCE_M * CLOSEST_DISTANCE_M);
    const float radialVelocity = x * velocityX / distance; // negative = approaching
    const float denominator = SPEED_OF_SOUND_MPS + radialVelocity;
    const float dopplerRate = SPEED_OF_SOUND_MPS / denominator;

    helicopter.setPlaybackRate(dopplerRate);
    pan.setPosition(x / HALF_SPAN_M, CLOSEST_DISTANCE_M / distance);

    vTaskDelay(pdMS_TO_TICKS(10));
}
