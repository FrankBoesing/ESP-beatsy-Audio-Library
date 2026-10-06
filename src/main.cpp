
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
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("Connecting to Wi-Fi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(200);
        Serial.print('.');
    }
    Serial.printf("\nConnected, IP: %s\n", WiFi.localIP().toString().c_str());

    // The stream endpoint uses HTTPS; certificate validation is disabled here.
    tlsClient.setInsecure();
    http.useHTTP10(true);
    http.setReuse(true);
    http.setUserAgent("VLC/3.0.21");
    http.addHeader("Accept", "*/*");
    http.addHeader("Icy-MetaData", "1");
    http.setTimeout(15000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setRedirectLimit(5);
    const char *responseHeaders[] = {"icy-metaint"};
    http.collectHeaders(responseHeaders, 1);

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

    uint32_t icyMetaInt = 0;
    String icyMetaIntHeader = http.header("icy-metaint");
    icyMetaIntHeader.trim();
    if (!icyMetaIntHeader.isEmpty()) {
        bool validHeader = true;
        for (size_t i = 0; i < icyMetaIntHeader.length(); ++i) {
            const char c = icyMetaIntHeader[i];
            if (c < '0' || c > '9') {
                validHeader = false;
                break;
            }
        }

        errno = 0;
        char *end = nullptr;
        const unsigned long parsed = strtoul(icyMetaIntHeader.c_str(), &end, 10);
        if (!validHeader || errno == ERANGE || end == icyMetaIntHeader.c_str() ||
            *end != '\0' || parsed == 0 || parsed > UINT32_MAX) {
            Serial.printf("Invalid icy-metaint response header: %s\n",
                          icyMetaIntHeader.c_str());
            http.end();
            stopWithError("ERROR: Invalid ICY metadata interval");
        }
        icyMetaInt = (uint32_t)(parsed);
        Serial.printf("ICY metadata interval: %lu audio bytes\n",
                      (unsigned long)(icyMetaInt));
    }

    if (!audioSource.open(http.getStream(), icyMetaInt)) {
        http.end();
        stopWithError("ERROR: Could not start stream buffer");
    }

    Serial.println("Buffering stream...");
    const uint32_t bufferDeadline = millis() + 30000;
    while (audioSource.bufferedBytes() < 32U * 1024U &&
           (int32_t)(bufferDeadline - millis()) > 0) {
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
    static uint8_t icyMetadata[AudioSourceStream::ICY_METADATA_MAX_SIZE];

    size_t icyMetadataSize = 0;
    if (audioSource.takeIcyMetadata(icyMetadata, sizeof(icyMetadata), icyMetadataSize)) {
        while (icyMetadataSize > 0 && icyMetadata[icyMetadataSize - 1] == 0) {
            --icyMetadataSize;
        }

        if (icyMetadataSize > 0) {
            Serial.print("ICY metadata: ");
            Serial.write(icyMetadata, icyMetadataSize);
            Serial.println();
        }
    }

    if (millis() - lastStatus >= 5000) {
        lastStatus = millis();
        const float core0Usage = AudioStream::processorUsage(0);
        const float core1Usage = AudioStream::processorUsage(1);

        Serial.printf("Wi-Fi: %s (RSSI %d dBm), MP3: %s (error %d)\n",
                      WiFi.status() == WL_CONNECTED ? "connected" : "disconnected",
                      WiFi.RSSI(), mp3.isPlaying() ? "playing" : "waiting",
                      mp3.lastError());
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
    }

    vTaskDelay(200);
}
