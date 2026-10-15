#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <math.h>
#include <Audio.h>

// CC0 sample: "Helicopter" by mrspivey (Freesound 805192).
// Download the original 44.1 kHz / 16-bit stereo WAV and copy it to the SD card
// as /Helicopter.wav. See the accompanying README for the source and license.

namespace {
constexpr char SAMPLE_FILE[] = "/Helicopter.wav";

constexpr float SPEED_OF_SOUND_MPS = 343.0f;
constexpr float HALF_SPAN_M = 180.0f;    // Far-right to far-left distance
constexpr float CLOSEST_DISTANCE_M = 35.0f;
constexpr uint32_t LEG_DURATION_MS = 15500; // One direction; close to sample length
constexpr float PI_F = 3.14159265358979323846f;

uint32_t retryStartAt = 0;

bool mountSD() {
#if defined(PIN_SD_CS) && defined(PIN_SPI_MOSI) && defined(PIN_SPI_MISO) && defined(PIN_SPI_SCK)
    SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_SD_CS);
    return SD.begin(PIN_SD_CS, SPI);
#else
    return SD.begin();
#endif
}
} // namespace

// Small example-local stereo-to-mono-to-stereo panner. The source's two
// channels are summed to mono so the flight model, not the source recording,
// controls the left/right movement.
class AudioEffectDopplerPan : public AudioStream {
public:
    AudioEffectDopplerPan() : AudioStream(2, inputQueueArray) {}

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
        audio_block_t *inputLeft = receiveReadOnly(0);
        audio_block_t *inputRight = receiveReadOnly(1);
        if (inputLeft == nullptr || inputRight == nullptr) {
            if (inputLeft != nullptr) release(inputLeft);
            if (inputRight != nullptr) release(inputRight);
            return;
        }

        audio_block_t *left = allocate();
        audio_block_t *right = allocate();
        if (left == nullptr || right == nullptr) {
            if (left != nullptr) release(left);
            if (right != nullptr) release(right);
            release(inputLeft);
            release(inputRight);
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
            const int32_t mono =
                ((int32_t)inputLeft->data[i] + (int32_t)inputRight->data[i]) / 2;
            left->data[i] = (int16_t)(mono * leftGain);
            right->data[i] = (int16_t)(mono * rightGain);
        }

        transmit(left, 0);
        transmit(right, 1);
        release(left);
        release(right);
        release(inputLeft);
        release(inputRight);
    }

private:
    audio_block_t *inputQueueArray[2] = {};
    portMUX_TYPE controlMux = portMUX_INITIALIZER_UNLOCKED;
    float targetPan = 1.0f;
    float targetGain = 0.0f;
    float currentPan = 1.0f;
    float currentGain = 0.0f;
};

AudioPlaySdWav helicopter;
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

AudioConnection patchCordSourceLeft(helicopter, 0, pan, 0);
AudioConnection patchCordSourceRight(helicopter, 1, pan, 1);
AudioConnection patchCordLeft(pan, 0, output, 0);
AudioConnection patchCordRight(pan, 1, output, 1);

static bool startHelicopter() {
    if (!helicopter.play(SD, SAMPLE_FILE)) {
        Serial.printf("ERROR: Cannot play %s. Check the SD card and WAV format.\n", SAMPLE_FILE);
        retryStartAt = millis() + 1000U;
        return false;
    }

    Serial.printf("Playing %s\n", SAMPLE_FILE);
    retryStartAt = 0;
    return true;
}

void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println();
    Serial.println("======================================");
    Serial.println(" Doppler Helicopter");
    Serial.println("======================================");

    if (!mountSD()) {
        Serial.println("ERROR: SD (SPI) initialization failed. Check the card and platformio.ini pins.");
        while (true) delay(1000);
    }

    AudioMemory(16);
    output.setChannelCount(codec.channels());

    if (!codec.enable()) {
        Serial.println("ERROR: Audio codec initialization failed");
        while (true) delay(1000);
    }
    codec.volume(0.65f);

    helicopter.setPlaybackRate(1.0f);
    pan.setPosition(1.0f, 0.0f);
    startHelicopter();
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
    const float direction = flyingLeft ? 1.0f : -1.0f;
    const float x = direction * HALF_SPAN_M * cosf(angle);
    const float velocityX = flyingLeft
                                ? -HALF_SPAN_M * PI_F / (LEG_DURATION_MS / 1000.0f) * sinf(angle)
                                :  HALF_SPAN_M * PI_F / (LEG_DURATION_MS / 1000.0f) * sinf(angle);

    const float distance = sqrtf(x * x + CLOSEST_DISTANCE_M * CLOSEST_DISTANCE_M);
    const float radialVelocity = x * velocityX / distance; // negative = approaching
    const float denominator = SPEED_OF_SOUND_MPS + radialVelocity;
    const float dopplerRate = SPEED_OF_SOUND_MPS / denominator;

    // The rate multiplier changes both playback speed and pitch. The panner and
    // inverse-distance gain supply the movement cues alongside the Doppler shift.
    helicopter.setPlaybackRate(dopplerRate);
    pan.setPosition(x / HALF_SPAN_M, CLOSEST_DISTANCE_M / distance);

    // The source recording is loopable. Restart it at EOF; the flight model
    // continues independently, so the turnaround does not depend on file I/O.
    if (!helicopter.isPlaying() &&
        (retryStartAt == 0 || (int32_t)(now - retryStartAt) >= 0)) {
        startHelicopter();
    }

    vTaskDelay(pdMS_TO_TICKS(10));
}
