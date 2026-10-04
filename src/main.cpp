
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_heap_caps.h>

#include "Audio.h"
#include "softcodecs/AudioSourceStream.h"

constexpr char WIFI_SSID[] = "Abschirmdienst";
constexpr char WIFI_PASSWORD[] = "frank123";
//constexpr char STREAM_URL[] = "https://st01.sslstream.dlf.de/dlf/01/128/mp3/stream.mp3";
//constexpr char STREAM_URL[] = "https://st01.sslstream.dlf.de/dlf/01/mid/aac/stream.aac";
constexpr char STREAM_URL[] = "https://st01.sslstream.dlf.de/dlf/01/high/aac/stream.aac";

AudioControlES8388 codec;
AudioPlayAac mp3;
AudioSourceStream audioSource;
WiFiClientSecure tlsClient;
HTTPClient http;

AudioOutputI2S i2s({
    27, // BCLK
    25, // WS / LRCLK
    26, // DOUT
    0   // MCLK
});

AudioConnection patchCordLeft(mp3, 0, i2s, 0);
AudioConnection patchCordRight(mp3, 1, i2s, 1);

void printFreeRam() {
    Serial.printf("Free heap: internal %u B (largest block %u B, min ever %u B), PSRAM %u B\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

[[noreturn]] void stopWithError(const char *message) {
    Serial.println(message);
    while (true) {
        delay(1000);
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    AudioMemory(10);

    Serial.println("Initializing ES8388...");
    if (!codec.enable()) {
        stopWithError("ERROR: ES8388 initialization failed");
    }
    codec.volume(0.7f);

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("Connecting to Wi-Fi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print('.');
    }
    Serial.printf("\nConnected, IP: %s\n", WiFi.localIP().toString().c_str());

    // The stream endpoint uses HTTPS; certificate validation is disabled here.
    tlsClient.setInsecure();
    http.useHTTP10(true);

    http.setTimeout(15000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setRedirectLimit(5);

    Serial.println("Connecting to Deutschlandfunk stream...");
    if (!http.begin(tlsClient, STREAM_URL)) {
        stopWithError("ERROR: Could not initialize HTTP connection");
    }

    const int responseCode = http.GET();
    if (responseCode != HTTP_CODE_OK) {
        Serial.printf("HTTP error: %d\n", responseCode);
        http.end();
        stopWithError("ERROR: MP3 stream request failed");
    }

    if (!audioSource.open(http.getStream())) {
        http.end();
        stopWithError("ERROR: Could not start stream buffer");
    }

    Serial.println("Buffering stream...");
    const uint32_t bufferDeadline = millis() + 30000;
    while (audioSource.bufferedBytes() < 32U * 1024U &&
           static_cast<int32_t>(bufferDeadline - millis()) > 0) {
        delay(10);
    }
    if (audioSource.bufferedBytes() < 32U * 1024U) {
        audioSource.close();
        http.end();
        stopWithError("ERROR: Stream prebuffer timed out");
    }

    if (!mp3.play(audioSource)) {
        audioSource.close();
        http.end();
        stopWithError("ERROR: MP3 playback could not be started");
    }

    Serial.println("Deutschlandfunk stream playback started");
    printFreeRam();
}

void loop() {
    static uint32_t lastStatus = 0;
    static uint64_t lastBytes = 0;
    static uint32_t lastWouldBlockCount = 0;
    static uint32_t lastPcmUnderrunFrames = 0;
    static uint32_t lastNetworkWaitMs = 0;

    if (millis() - lastStatus >= 5000) {
        const uint32_t now = millis();
        const uint32_t elapsed = now - lastStatus;
        lastStatus = now;

        const uint64_t bytes = audioSource.receivedBytes();
        const uint32_t wouldBlockCount = audioSource.wouldBlockCount();
        const uint32_t pcmUnderrunFrames = mp3.pcmUnderrunFrames();
        const uint32_t networkWaitMs = audioSource.networkWaitMs();
        const float core0Usage = AudioStream::processorUsage(0);
        const float core1Usage = AudioStream::processorUsage(1);
        const double inputKbps = elapsed > 0 ? (bytes - lastBytes) * 8.0f / elapsed : 0.0;

        Serial.printf("Wi-Fi: %s (RSSI %d dBm), MP3: %s (error %d), "
                  "input: %.1f kbit/s\n",
                      WiFi.status() == WL_CONNECTED ? "connected" : "disconnected",
                      WiFi.RSSI(), mp3.isPlaying() ? "playing" : "waiting",
                  mp3.lastError(),
                      inputKbps);
        Serial.printf("5s deltas: input empty=%lu, network wait=%lu"
                  " ms, PCM silence=%lu frames, received=%llu"
                  " bytes, buffer=%u/%u KiB\n",
                      wouldBlockCount - lastWouldBlockCount,
                      networkWaitMs - lastNetworkWaitMs,
                      pcmUnderrunFrames - lastPcmUnderrunFrames, bytes,
                      audioSource.bufferedBytes() / 1024,
                      AudioSourceStream::BUFFER_SIZE / 1024);
#if SOFTCODEC_METRICS
        Serial.printf("Decoder load: avg %.2f%%, max %.2f%%\n",
                      mp3.decodeProcessorUsage(),
                      mp3.decodeProcessorUsageMax());
#endif
        if (core0Usage < 0.0f || core1Usage < 0.0f) {
            Serial.println("CPU cores: sampling...");
        } else {
            Serial.printf("CPU cores: core 0 %.1f%%, core 1 %.1f%%\n",
                          core0Usage, core1Usage);
        }

        lastBytes = bytes;
        lastWouldBlockCount = wouldBlockCount;
        lastPcmUnderrunFrames = pcmUnderrunFrames;
        lastNetworkWaitMs = networkWaitMs;
    }

    vTaskDelay(200);
}
