
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_heap_caps.h>
#include <cerrno>
#include <cstdlib>

#include "Audio.h"
#include "softcodecs/AudioSourceStream.h"
#include "secrets.h"
#include "radiohelpers.h"

const char TAG[] = "MAIN";

constexpr char STREAM_URL[] = "https://st01.sslstream.dlf.de/dlf/01/128/mp3/stream.mp3";
//constexpr char STREAM_URL[] = "https://st01.sslstream.dlf.de/dlf/01/mid/aac/stream.aac";
//constexpr char STREAM_URL[] = "https://st01.sslstream.dlf.de/dlf/01/high/aac/stream.aac";
//constexpr char STREAM_URL[] = "https://wdr-wdr2-rheinruhr.icecastssl.wdr.de/wdr/wdr2/rheinruhr/mp3/128/stream.mp3";
//constexpr char STREAM_URL[] = "https://mp3.ffh.de/radioffh/hqlivestream.aac";

AudioPlayMp3 *mp3 = nullptr;
AudioPlayAac *aac = nullptr;
AudioSourceStream audioSource;
WiFiClientSecure tlsClient;
HTTPClient http;

AudioOutputI2S i2s({
    PIN_I2S_BLCK, // BCLK
    PIN_I2S_WS,   // WS / LRCLK
    PIN_I2S_DOUT, // DOUT
    PIN_I2S_MLCK  // MCLK
});

#if defined(AUDIO_CODEC_ES8388)
//AudioControlES8388 codec({ PIN_I2C_SDA, PIN_I2C_SCL, PIN_AMPLIFIER, PIN_AMPLIFIER_ACTIVE});
#elif defined(AUDIO_CODEC_ES8311)
AudioControlES8311 codec({PIN_I2C_SDA, PIN_I2C_SCL, PIN_AMPLIFIER, PIN_AMPLIFIER_ACTIVE});
#else
#error "AUDIO_CODEC not defined"
#endif

AudioConnection *patchCordLeft = nullptr;
AudioConnection *patchCordRight = nullptr;

Radio::AudioCodec selectedCodec = Radio::AudioCodec::Unknown;

bool onStreamSampleRate(uint32_t rate, void *) {
    ESP_LOGI(TAG, "Switching output sample rate to %u Hz", (unsigned)rate);
    return i2s.setSampleRate((float)rate);
}

bool connectPlayer(AudioDecoderStream &player) {
    player.onSampleRateChange(onStreamSampleRate);
    patchCordLeft = new AudioConnection();
    patchCordRight = new AudioConnection();
    if (patchCordLeft == nullptr || patchCordRight == nullptr) {
        ESP_LOGE(TAG, "ERROR: Could not allocate audio connections");
        return false;
    }

    const int leftResult = patchCordLeft->connect(player, 0, i2s, 0);
    if (leftResult != 0) {
        ESP_LOGE(TAG, "ERROR: Left audio connection failed (%d)", leftResult);
        return false;
    }

    const int rightResult = patchCordRight->connect(player, 1, i2s, 1);
    if (rightResult != 0) {
        ESP_LOGE(TAG, "ERROR: Right audio connection failed (%d)", rightResult);
        return false;
    }

    return true;
}

void printFreeRam() {
    ESP_LOGI(TAG, "Free heap: internal %u B (largest block %u B, min ever %u B), PSRAM %u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

[[noreturn]] void stopWithError(const char *message) {
    ESP_LOGE(TAG, "%s", message);
    while (true) {
        delay(1000);
    }
}

void configStream(auto &http, bool useIcy = false, followRedirects_t followRedirects = HTTPC_STRICT_FOLLOW_REDIRECTS,
                  int redirectLimit = 5, int timeout = 15000) {
    http.useHTTP10(true);
    http.setUserAgent("VLC/3.0.21");
    http.addHeader("Accept", "*/*");

    const char *headerKeys[] = {"content-type", "icy-metaint"};
    http.collectHeaders(headerKeys, sizeof(headerKeys) / sizeof(char *));

    if (useIcy) http.addHeader("Icy-MetaData", "1");

    http.setTimeout(timeout);
    http.setFollowRedirects(followRedirects);
    http.setRedirectLimit(redirectLimit);
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    AudioMemory(10);

    ESP_LOGI(TAG, "Initializing codec...");
    if (!codec.enable()) {
        stopWithError("ERROR: codec initialization failed");
    }
    codec.volume(0.6f);

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    ESP_LOGI(TAG, "Connecting to Wi-Fi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(200);
        Serial.print('.');
    }
    ESP_LOGI(TAG, "\nConnected, IP: %s", WiFi.localIP().toString().c_str());

    // The stream endpoint uses HTTPS; certificate validation is disabled here.

    ESP_LOGI(TAG, "Connecting to radio stream...");
    tlsClient.setInsecure();
    configStream(http, true);

    if (!http.begin(tlsClient, STREAM_URL)) {
        stopWithError("ERROR: Could not initialize HTTP connection");
    }

    const int responseCode = http.GET();
    if (responseCode != HTTP_CODE_OK) {
        ESP_LOGD(TAG, "HTTP error: %d", responseCode);
        http.end();
        stopWithError("ERROR: Stream request failed");
    }

    selectedCodec = Radio::detectCodec(http); //see radiohelpers.h
    if (selectedCodec == Radio::AudioCodec::Unknown) {
        http.end();
        stopWithError("ERROR: Could not detect stream codec");
    }

    if (selectedCodec == Radio::AudioCodec::MP3) {
        mp3 = new AudioPlayMp3();
        if (mp3 == nullptr) {
            http.end();
            stopWithError("ERROR: Could not allocate MP3 player");
        }
        if (!connectPlayer(*mp3)) {
            http.end();
            stopWithError("ERROR: Could not connect MP3 player to I2S output");
        }
    } else {
        aac = new AudioPlayAac();
        if (aac == nullptr) {
            http.end();
            stopWithError("ERROR: Could not allocate AAC player");
        }
        if (!connectPlayer(*aac)) {
            http.end();
            stopWithError("ERROR: Could not connect AAC player to I2S output");
        }
    }

    if (!audioSource.open(http)) {
        http.end();
        stopWithError("ERROR: Could not start stream buffer");
    }

    ESP_LOGI(TAG, "Buffering stream...");
    const uint32_t bufferDeadline = millis() + 30000;
    while (audioSource.bufferedBytes() < 32U * 1024U && (int32_t)(bufferDeadline - millis()) > 0) {
        delay(10);
    }
    if (audioSource.bufferedBytes() < 32U * 1024U) {
        audioSource.close();
        http.end();
        stopWithError("ERROR: Stream prebuffer timed out");
    }

    const bool playbackStarted = selectedCodec == Radio::AudioCodec::MP3 ? mp3->play(audioSource) : aac->play(audioSource);
    if (!playbackStarted) {
        audioSource.close();
        http.end();
        stopWithError("ERROR: Stream playback could not be started");
    }

    ESP_LOGI(TAG, "Stream playback started");
    printFreeRam();
}

void loop() {
    static uint32_t lastStatus = 0;

    static char icyMetadata[AudioSourceStream::ICY_STREAMTITLE_MAX_SIZE];

    int icyMetadataLen = audioSource.takeIcyStreamTitle((char *)icyMetadata, sizeof(icyMetadata));
    if (icyMetadataLen) {
        ESP_LOGI(TAG, "ICY metadata: %s", icyMetadata);
    }

    if (millis() - lastStatus >= 5000) {
        lastStatus = millis();
        const float core0Usage = AudioStream::processorUsage(0);
        const float core1Usage = AudioStream::processorUsage(1);

        const bool isPlaying = selectedCodec == Radio::AudioCodec::MP3 ? mp3->isPlaying() : aac->isPlaying();
        const int decoderError = selectedCodec == Radio::AudioCodec::MP3 ? mp3->lastError() : aac->lastError();
        const char *codecName = Radio::codecName(selectedCodec);
        ESP_LOGI(TAG, "Wi-Fi: %s (RSSI %d dBm), %s: %s (error %d)",
                 WiFi.status() == WL_CONNECTED ? "connected" : "disconnected", WiFi.RSSI(), codecName,
                 isPlaying ? "playing" : "waiting", decoderError);
#if SOFTCODEC_METRICS
        if (selectedCodec == Radio::AudioCodec::MP3) {
            ESP_LOGI(TAG, "Decoder load: avg %.2f%%, max %.2f%%", mp3->decodeProcessorUsage(),
                     mp3->decodeProcessorUsageMax());
        } else if (selectedCodec == Radio::AudioCodec::AAC) {
            ESP_LOGI(TAG, "Decoder load: avg %.2f%%, max %.2f%%", aac->decodeProcessorUsage(),
                     aac->decodeProcessorUsageMax());
        }
#endif
        if (core0Usage < 0.0f || core1Usage < 0.0f) {
            ESP_LOGI(TAG, "CPU cores: sampling...");
        } else {
            ESP_LOGI(TAG, "CPU cores: core 0 %.1f%%, core 1 %.1f%%", core0Usage, core1Usage);
        }
    }

    vTaskDelay(200);
}
